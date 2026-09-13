#include "AsioPcmOutput.h"
#include "BufferNudgeController.h"

#include <asiodrivers.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>

// Supplied by Steinberg's host/asiodrivers.cpp.
extern bool loadAsioDriver(char* name);
extern AsioDrivers* asioDrivers;

std::atomic<AsioPcmOutput*> AsioPcmOutput::activeInstance_{ nullptr };

namespace
{
std::string wideToAnsi(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), bytes, nullptr, nullptr);
    return out;
}
}

AsioPcmOutput::AsioPcmOutput()
    : nudgeController_(std::make_unique<BufferNudgeController>())
{
    callbacks_.bufferSwitch = &AsioPcmOutput::bufferSwitch;
    callbacks_.sampleRateDidChange = &AsioPcmOutput::sampleRateDidChange;
    callbacks_.asioMessage = &AsioPcmOutput::asioMessages;
    callbacks_.bufferSwitchTimeInfo = &AsioPcmOutput::bufferSwitchTimeInfo;

    thread_ = std::thread([this]() { workerLoop(); });
}

AsioPcmOutput::~AsioPcmOutput()
{
    stop_.store(true, std::memory_order_release);
    condition_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

std::vector<AsioOutputDriver> AsioPcmOutput::enumerateDrivers()
{
    std::vector<AsioOutputDriver> result;
    AsioDrivers drivers;
    constexpr long MaxDrivers = 64;
    char names[MaxDrivers][64]{};
    char* namePtrs[MaxDrivers]{};
    for (long i = 0; i < MaxDrivers; ++i)
        namePtrs[i] = names[i];
    const long count = drivers.getDriverNames(namePtrs, MaxDrivers);
    for (long i = 0; i < count; ++i)
        result.push_back({ names[i] });
    return result;
}

void AsioPcmOutput::setEnabled(bool enabled)
{
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled)
        clear();
    requestRestart();
}

void AsioPcmOutput::setDriverName(const std::string& driverNameUtf8)
{
    {
        std::lock_guard lock(configMutex_);
        if (driverNameUtf8_ == driverNameUtf8)
            return;
        driverNameUtf8_ = driverNameUtf8;
    }
    clear();
    requestRestart();
}


double AsioPcmOutput::targetMs() const noexcept
{
    const double manual = targetOverrideMs_.load(std::memory_order_acquire);
    if (manual > 0.0)
        return manual;
    const double q = bufferPeriodMs_.load(std::memory_order_acquire);
    return q > 0.0 ? 4.0 * q : 0.0;
}

void AsioPcmOutput::setTargetMs(double milliseconds)
{
    targetOverrideMs_.store(milliseconds > 0.0 ? milliseconds : 0.0, std::memory_order_release);
    nudgeController_->reset();
}

std::string AsioPcmOutput::driverName() const
{
    std::lock_guard lock(configMutex_);
    return driverNameUtf8_;
}

void AsioPcmOutput::setBufferLoggingEnabled(bool enabled)
{
    bufferLoggingEnabled_.store(enabled, std::memory_order_release);

    std::lock_guard logLock(logMutex_);
    if (!enabled)
    {
        if (bufferLog_.is_open())
        {
            bufferLog_ << "ASIO BUFFER LOG STOP\n";
            bufferLog_.flush();
            bufferLog_.close();
        }
        return;
    }

    if (!bufferLog_.is_open())
    {
        bufferLog_.open("asio_buffer.log", std::ios::out | std::ios::trunc);
        if (bufferLog_.is_open())
        {
            bufferLog_ << "OpenScope ASIO output buffer log\n";
            const auto qMs = bufferPeriodMs_.load(std::memory_order_acquire);
            const auto frames = bufferFrames_.load(std::memory_order_acquire);
            if (qMs > 0.0 && frames > 0)
            {
                bufferLog_ << "buffer=" << frames << " frames Q=" << std::fixed << std::setprecision(3)
                    << qMs << " ms target=" << targetMs() << " ms\n";
            }
            bufferLog_.flush();
        }
        logEpoch_ = std::chrono::steady_clock::now();
        lastLog_ = {};
        lastLoggedCallbacks_ = callbackCount_.load(std::memory_order_relaxed);
        lastLoggedUnderruns_ = underruns_.load(std::memory_order_relaxed);
        lastLoggedOverruns_ = overruns_.load(std::memory_order_relaxed);
        fillAveragesValid_ = false;
    }
}

void AsioPcmOutput::clear()
{
    std::lock_guard lock(fifoMutex_);
    fifo_.clear();
    sourcePhase_ = 0.0;
    lastPushTime_ = {};
    nudgeController_->reset();
    queuedMs_.store(0.0, std::memory_order_release);
    lowWaterMs_.store(0.0, std::memory_order_release);
    nudgePpm_.store(0.0, std::memory_order_release);
    fastFillMs_ = 0.0;
    avg3FillMs_ = 0.0;
    fillAveragesValid_ = false;
}


void AsioPcmOutput::pushStereo(const std::vector<std::int16_t>& interleavedStereo, double sourceSampleRate)
{
    if (!enabled_.load(std::memory_order_acquire) || interleavedStereo.empty() || sourceSampleRate <= 1000.0)
        return;

    const auto now = std::chrono::steady_clock::now();
    bool rateChanged = false;

    {
        std::lock_guard lock(configMutex_);
        if (std::abs(requestedSourceRate_ - sourceSampleRate) > 0.5)
        {
            requestedSourceRate_ = sourceSampleRate;
            rateChanged = true;
        }
    }

    if (rateChanged)
    {
        clear();
        requestRestart();
    }

    std::lock_guard lock(fifoMutex_);
    sourceRate_ = sourceSampleRate;

    // Keep the pre-refill reserve bounded. PCM arrives in roughly one video
    // frame bursts, so this cap applies to the low-water reserve, not to the
    // short post-refill peak. If Windows stalls us for a while, discard old
    // delayed audio rather than building a hundreds-of-ms latency account.
    constexpr double MaxLowWaterReserveMs = 40.0;
    const std::size_t maxReserveFrames = static_cast<std::size_t>(
        std::ceil(sourceSampleRate * MaxLowWaterReserveMs / 1000.0));
    const std::size_t maxReserveSamples = maxReserveFrames * 2;
    bool reserveTrimmed = false;
    while (fifo_.size() > maxReserveSamples)
    {
        fifo_.pop_front();
        if (!fifo_.empty())
            fifo_.pop_front();
        reserveTrimmed = true;
    }
    if (reserveTrimmed)
        overruns_.fetch_add(1, std::memory_order_relaxed);

    const double beforeFrames = static_cast<double>(fifo_.size() / 2);
    const double beforeMs = 1000.0 * beforeFrames / sourceSampleRate;
    lowWaterMs_.store(beforeMs, std::memory_order_release);
    const double qMs = bufferPeriodMs_.load(std::memory_order_acquire);
    if (qMs > 0.0)
    {
        // If a scheduler hiccup kicked us far out of the settled band, do not
        // remain in the slow maintenance ladder. Re-enter hard acquisition.
        if (!nudgeController_->acquisitionActive() &&
            std::abs(beforeMs - targetMs()) > 9.0 * qMs)
        {
            nudgeController_->forceAcquisition();
        }
        double dt = 0.020;
        if (lastPushTime_.time_since_epoch().count() != 0)
            dt = std::chrono::duration<double>(now - lastPushTime_).count();
        const double target = targetMs();
        nudgeController_->update(beforeMs, target, dt, qMs);
        nudgePpm_.store(nudgeController_->appliedPpm(), std::memory_order_release);
    }
    lastPushTime_ = now;

    for (const auto sample : interleavedStereo)
        fifo_.push_back(sample);

    const std::size_t maxSamples = static_cast<std::size_t>(std::ceil(sourceSampleRate * 0.5)) * 2;
    bool overflowed = false;
    while (fifo_.size() > maxSamples)
    {
        fifo_.pop_front();
        if (!fifo_.empty())
            fifo_.pop_front();
        overflowed = true;
    }
    if (overflowed)
        overruns_.fetch_add(1, std::memory_order_relaxed);

    const double postMs = 1000.0 * static_cast<double>(fifo_.size() / 2) / sourceSampleRate;
    queuedMs_.store(postMs, std::memory_order_release);

    // Keep the live fill averages available even when file logging is disabled.
    if (!fillAveragesValid_)
    {
        fastFillMs_ = beforeMs;
        avg3FillMs_ = beforeMs;
        fillAveragesValid_ = true;
    }
    else
    {
        const double dt = 0.040; // PCM arrives once per PAL frame.
        const double aFast = 1.0 - std::exp(-dt / 0.25);
        const double a3 = 1.0 - std::exp(-dt / 3.0);
        fastFillMs_ += aFast * (beforeMs - fastFillMs_);
        avg3FillMs_ += a3 * (beforeMs - avg3FillMs_);
    }
    fastFillAtomic_.store(fastFillMs_, std::memory_order_release);
    avg3FillAtomic_.store(avg3FillMs_, std::memory_order_release);

    // Diagnostics are deliberately written from the producer side only.
    // The ASIO callback merely increments atomics so disk I/O can never block it.
    if (bufferLoggingEnabled_.load(std::memory_order_acquire))
    {
        std::lock_guard logLock(logMutex_);
        const auto cb = callbackCount_.load(std::memory_order_relaxed);
        const auto und = underruns_.load(std::memory_order_relaxed);
        const auto ov = overruns_.load(std::memory_order_relaxed);
        const bool counterEvent = und != lastLoggedUnderruns_ || ov != lastLoggedOverruns_;
        const bool due = lastLog_.time_since_epoch().count() == 0 ||
            std::chrono::duration<double>(now - lastLog_).count() >= 1.0;

        if (bufferLog_.is_open() && (due || counterEvent))
        {
            const double elapsed = logEpoch_.time_since_epoch().count() == 0
                ? 0.0 : std::chrono::duration<double>(now - logEpoch_).count();
            double cbRate = 0.0;
            if (lastLog_.time_since_epoch().count() != 0)
            {
                const double logDt = std::chrono::duration<double>(now - lastLog_).count();
                if (logDt > 0.0) cbRate = static_cast<double>(cb - lastLoggedCallbacks_) / logDt;
            }
            bufferLog_ << std::fixed << std::setprecision(3) << elapsed
                << "  BUFFER rawLow=" << beforeMs << " ms"
                << " fast=" << fastFillMs_ << " ms"
                << " avg3=" << avg3FillMs_ << " ms"
                << " postFill=" << postMs << " ms"
                << " target=" << targetMs() << " ms"
                << " Q=" << qMs << " ms"
                << " ppm=" << std::setprecision(1) << nudgePpm_.load(std::memory_order_relaxed)
                << " acquire=" << (nudgeController_->acquisitionActive() ? 1 : 0)
                << " cb=" << cbRate << "/s"
                << " underruns=" << und
                << " overruns=" << ov
                << '\n';
            bufferLog_.flush();
            lastLog_ = now;
            lastLoggedCallbacks_ = cb;
            lastLoggedUnderruns_ = und;
            lastLoggedOverruns_ = ov;
        }
    }
}

void AsioPcmOutput::requestRestart()
{
    restartRequested_.store(true, std::memory_order_release);
    condition_.notify_all();
}

void AsioPcmOutput::workerLoop()
{
    // The ASIO control/feed thread must not be background-throttled when the
    // OpenScope window is minimized or another full-screen utility is active.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    while (!stop_.load(std::memory_order_acquire))
    {
        std::unique_lock waitLock(conditionMutex_);
        const bool woke = condition_.wait_for(waitLock, std::chrono::milliseconds(100), [this]()
        {
            return stop_.load(std::memory_order_acquire) || restartRequested_.load(std::memory_order_acquire);
        });
        waitLock.unlock();

        if (stop_.load(std::memory_order_acquire))
            break;
        if (!woke || !restartRequested_.exchange(false, std::memory_order_acq_rel))
            continue;

        closeDriver();

        if (!enabled_.load(std::memory_order_acquire))
            continue;

        std::string driver;
        double rate = 0.0;
        {
            std::lock_guard lock(configMutex_);
            driver = driverNameUtf8_;
            rate = requestedSourceRate_;
        }
        if (driver.empty() || rate <= 1000.0)
            continue;

        openDriver(driver, rate);
    }

    closeDriver();
    SetThreadExecutionState(ES_CONTINUOUS);
}

bool AsioPcmOutput::openDriver(const std::string& name, double requestedRate)
{
    std::string mutableName = name;
    if (!loadAsioDriver(mutableName.data()))
        return false;
    driverLoaded_ = true;

    driverInfo_ = {};
    driverInfo_.asioVersion = 2;
    driverInfo_.sysRef = GetDesktopWindow();
    ASIOError err = ASIOInit(&driverInfo_);
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        closeDriver();
        return false;
    }
    asioInitialized_ = true;

    long inputs = 0;
    long outputs = 0;
    err = ASIOGetChannels(&inputs, &outputs);
    if ((err != ASE_OK && err != ASE_SUCCESS) || outputs < 2)
    {
        closeDriver();
        return false;
    }

    ASIOSampleRate currentRate = 0.0;
    if (ASIOGetSampleRate(&currentRate) != ASE_OK || std::abs(currentRate - requestedRate) > 0.5)
    {
        if (ASIOCanSampleRate(requestedRate) == ASE_OK)
            ASIOSetSampleRate(requestedRate);
        ASIOGetSampleRate(&currentRate);
    }
    if (!std::isfinite(currentRate) || currentRate <= 1000.0)
    {
        closeDriver();
        return false;
    }
    asioRate_ = currentRate;
    asioRateAtomic_.store(asioRate_, std::memory_order_release);

    long minSize = 0;
    long maxSize = 0;
    long preferredSize = 0;
    long granularity = 0;
    err = ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity);
    if ((err != ASE_OK && err != ASE_SUCCESS) || preferredSize <= 0)
    {
        closeDriver();
        return false;
    }

    const long bufferSize = preferredSize;
    bufferFrames_.store(bufferSize, std::memory_order_release);
    const double qMs = 1000.0 * static_cast<double>(bufferSize) / asioRate_;
    bufferPeriodMs_.store(qMs, std::memory_order_release);
    defaultTargetMs_.store(static_cast<int>(std::lround(4.0 * qMs)), std::memory_order_release);

    bufferInfos_.assign(2, {});
    channelInfos_.assign(2, {});
    for (long c = 0; c < 2; ++c)
    {
        bufferInfos_[static_cast<std::size_t>(c)].isInput = ASIOFalse;
        bufferInfos_[static_cast<std::size_t>(c)].channelNum = c;
        channelInfos_[static_cast<std::size_t>(c)].channel = c;
        channelInfos_[static_cast<std::size_t>(c)].isInput = ASIOFalse;
        if (ASIOGetChannelInfo(&channelInfos_[static_cast<std::size_t>(c)]) != ASE_OK)
        {
            closeDriver();
            return false;
        }
    }

    AsioPcmOutput* expected = nullptr;
    if (!activeInstance_.compare_exchange_strong(expected, this, std::memory_order_acq_rel))
    {
        closeDriver();
        return false;
    }

    err = ASIOCreateBuffers(bufferInfos_.data(), 2, bufferSize, &callbacks_);
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        closeDriver();
        return false;
    }
    buffersCreated_ = true;

    render(0);
    render(1);

    err = ASIOStart();
    if (err != ASE_OK && err != ASE_SUCCESS)
    {
        closeDriver();
        return false;
    }
    asioStarted_ = true;

    callbackCount_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    lastLoggedCallbacks_ = 0;
    lastLoggedUnderruns_ = 0;
    lastLoggedOverruns_ = 0;
    logEpoch_ = std::chrono::steady_clock::now();
    lastLog_ = {};
    fillAveragesValid_ = false;
    {
        std::lock_guard logLock(logMutex_);
        if (bufferLog_.is_open())
            bufferLog_.close();
        if (bufferLoggingEnabled_.load(std::memory_order_acquire))
        {
            bufferLog_.open("asio_buffer.log", std::ios::out | std::ios::trunc);
            if (bufferLog_.is_open())
            {
                bufferLog_ << "OpenScope ASIO output buffer log\n"
                    << "driver=\"" << name << "\" rate=" << std::fixed << std::setprecision(3)
                    << asioRate_ << " Hz buffer=" << bufferSize << " frames Q=" << qMs
                    << " ms target=" << targetMs() << " ms\n";
                bufferLog_.flush();
            }
        }
    }
    return true;
}

void AsioPcmOutput::closeDriver()
{
    if (asioStarted_)
    {
        ASIOStop();
        asioStarted_ = false;
    }
    if (buffersCreated_)
    {
        ASIODisposeBuffers();
        buffersCreated_ = false;
    }
    AsioPcmOutput* self = this;
    activeInstance_.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    if (asioInitialized_)
    {
        // Steinberg's Windows helper removes the loaded driver from ASIOExit().
        ASIOExit();
        asioInitialized_ = false;
        driverLoaded_ = false;
    }
    else if (driverLoaded_)
    {
        if (asioDrivers != nullptr)
            asioDrivers->removeCurrentDriver();
        driverLoaded_ = false;
    }
    {
        std::lock_guard logLock(logMutex_);
        if (bufferLog_.is_open())
        {
            bufferLog_ << "ASIO STOP\n";
            bufferLog_.flush();
            bufferLog_.close();
        }
    }
    bufferInfos_.clear();
    channelInfos_.clear();
    asioRate_ = 0.0;
    asioRateAtomic_.store(0.0, std::memory_order_release);
    bufferFrames_.store(0, std::memory_order_release);
    bufferPeriodMs_.store(0.0, std::memory_order_release);
    defaultTargetMs_.store(0, std::memory_order_release);
}

void AsioPcmOutput::bufferSwitch(long doubleBufferIndex, ASIOBool)
{
    // ASIO drivers normally invoke us on their own callback thread. Raise that
    // actual callback thread once, so minimize/full-screen GUI activity cannot
    // demote the audio deadline path behind rendering work.
    thread_local bool priorityRaised = false;
    if (!priorityRaised)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        priorityRaised = true;
    }

    if (auto* self = activeInstance_.load(std::memory_order_acquire))
        self->render(doubleBufferIndex);
}

ASIOTime* AsioPcmOutput::bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
{
    bufferSwitch(doubleBufferIndex, directProcess);
    return params;
}

void AsioPcmOutput::sampleRateDidChange(ASIOSampleRate)
{
    if (auto* self = activeInstance_.load(std::memory_order_acquire))
        self->requestRestart();
}

long AsioPcmOutput::asioMessages(long selector, long, void*, double*)
{
    switch (selector)
    {
    case kAsioSelectorSupported:
    case kAsioEngineVersion:
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioResetRequest:
    case kAsioBufferSizeChange:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        if (auto* self = activeInstance_.load(std::memory_order_acquire))
            self->requestRestart();
        return 1;
    default:
        return 0;
    }
}

void AsioPcmOutput::writeSample(void* destination, ASIOSampleType type, double value) const
{
    value = std::clamp(value, -1.0, 0.999999999);
    switch (type)
    {
    case ASIOSTInt16LSB:
        *reinterpret_cast<std::int16_t*>(destination) = static_cast<std::int16_t>(std::lround(value * 32767.0));
        break;
    case ASIOSTInt24LSB:
    {
        const std::int32_t s = static_cast<std::int32_t>(std::lround(value * 8388607.0));
        auto* p = reinterpret_cast<std::uint8_t*>(destination);
        p[0] = static_cast<std::uint8_t>(s & 0xff);
        p[1] = static_cast<std::uint8_t>((s >> 8) & 0xff);
        p[2] = static_cast<std::uint8_t>((s >> 16) & 0xff);
        break;
    }
    case ASIOSTInt32LSB:
        *reinterpret_cast<std::int32_t*>(destination) = static_cast<std::int32_t>(std::llround(value * 2147483647.0));
        break;
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        const int bits = type == ASIOSTInt32LSB16 ? 16 :
                         type == ASIOSTInt32LSB18 ? 18 :
                         type == ASIOSTInt32LSB20 ? 20 : 24;
        const double maxValue = static_cast<double>((std::uint64_t{1} << (bits - 1)) - 1);
        const auto sample = static_cast<std::int32_t>(std::llround(value * maxValue));
        *reinterpret_cast<std::int32_t*>(destination) = sample << (32 - bits);
        break;
    }
    case ASIOSTFloat32LSB:
        *reinterpret_cast<float*>(destination) = static_cast<float>(value);
        break;
    case ASIOSTFloat64LSB:
        *reinterpret_cast<double*>(destination) = value;
        break;
    default:
        std::memset(destination, 0, 4);
        break;
    }
}

void AsioPcmOutput::render(long doubleBufferIndex)
{
    callbackCount_.fetch_add(1, std::memory_order_relaxed);
    const long frames = bufferFrames_.load(std::memory_order_acquire);
    if (frames <= 0 || bufferInfos_.size() < 2 || channelInfos_.size() < 2)
        return;

    auto bytesPerSample = [](ASIOSampleType type) -> int
    {
        switch (type)
        {
        case ASIOSTInt16LSB: return 2;
        case ASIOSTInt24LSB: return 3;
        case ASIOSTInt32LSB: return 4;
        case ASIOSTInt32LSB16:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24: return 4;
        case ASIOSTFloat32LSB: return 4;
        case ASIOSTFloat64LSB: return 8;
        default: return 4;
        }
    };

    std::lock_guard lock(fifoMutex_);
    const double srcRate = sourceRate_ > 1000.0 ? sourceRate_ : asioRate_;
    const double ppm = nudgePpm_.load(std::memory_order_acquire);
    const double step = (srcRate / asioRate_) * (1.0 + ppm * 1.0e-6);
    bool underrun = false;

    for (long f = 0; f < frames; ++f)
    {
        double l = 0.0;
        double r = 0.0;
        if (fifo_.size() >= 4)
        {
            const double frac = sourcePhase_;
            const double l0 = static_cast<double>(fifo_[0]) / 32768.0;
            const double r0 = static_cast<double>(fifo_[1]) / 32768.0;
            const double l1 = static_cast<double>(fifo_[2]) / 32768.0;
            const double r1 = static_cast<double>(fifo_[3]) / 32768.0;
            l = l0 + (l1 - l0) * frac;
            r = r0 + (r1 - r0) * frac;

            sourcePhase_ += step;
            while (sourcePhase_ >= 1.0 && fifo_.size() >= 4)
            {
                fifo_.pop_front();
                fifo_.pop_front();
                sourcePhase_ -= 1.0;
            }
        }
        else
        {
            underrun = true;
        }

        for (int c = 0; c < 2; ++c)
        {
            const auto type = channelInfos_[static_cast<std::size_t>(c)].type;
            const int stride = bytesPerSample(type);
            auto* base = reinterpret_cast<std::uint8_t*>(bufferInfos_[static_cast<std::size_t>(c)].buffers[doubleBufferIndex]);
            writeSample(base + static_cast<std::size_t>(f) * stride, type, c == 0 ? l : r);
        }
    }

    if (underrun)
        underruns_.fetch_add(1, std::memory_order_acq_rel);

    if (srcRate > 1000.0)
        queuedMs_.store(1000.0 * static_cast<double>(fifo_.size() / 2) / srcRate, std::memory_order_release);

    ASIOOutputReady();
}
