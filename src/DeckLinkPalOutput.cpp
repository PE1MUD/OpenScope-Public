#include "DeckLinkPalOutput.h"

#include "DeckLinkApiWin.h"
#include "Pcm16VideoEncoder.h"
#include "HamPcmV2Encoder.h"
#include "AsioCapture.h"
#include "RealtimeThreadPolicy.h"

#include <cstring>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>

namespace
{
std::wstring nameOf(IDeckLink* device)
{
    BSTR name = nullptr;
    std::wstring result = L"Blackmagic output";

    if (SUCCEEDED(device->GetDisplayName(&name)) && name != nullptr)
    {
        result = name;
        SysFreeString(name);
    }

    return result;
}

bool playbackBusy(IDeckLink* device)
{
    if (device == nullptr)
        return false;

    int64_t busyState = 0;
    bool haveBusyState = false;

    IDeckLinkStatus* status = nullptr;
    if (SUCCEEDED(
            device->QueryInterface(
                IID_IDeckLinkStatus,
                reinterpret_cast<void**>(&status))) &&
        status != nullptr)
    {
        if (SUCCEEDED(
                status->GetInt(
                    bmdDeckLinkStatusBusy,
                    &busyState)))
        {
            haveBusyState = true;
        }

        status->Release();
    }

    // If another process/application already owns playback, it is never a
    // usable PCM output candidate.
    if (haveBusyState &&
        (busyState & bmdDevicePlaybackBusy) != 0)
    {
        return true;
    }

    // Capture only blocks playback on half-duplex/simplex hardware. Full-duplex
    // cards are explicitly allowed to capture and play back simultaneously.
    int64_t duplexValue = static_cast<int64_t>(bmdDuplexHalf);
    bool haveDuplex = false;

    IDeckLinkProfileAttributes* attributes = nullptr;
    if (SUCCEEDED(
            device->QueryInterface(
                IID_IDeckLinkProfileAttributes,
                reinterpret_cast<void**>(&attributes))) &&
        attributes != nullptr)
    {
        if (SUCCEEDED(
                attributes->GetInt(
                    BMDDeckLinkDuplex,
                    &duplexValue)))
        {
            haveDuplex = true;
        }

        attributes->Release();
    }

    const BMDDuplexMode duplex =
        static_cast<BMDDuplexMode>(duplexValue);

    if (haveDuplex && duplex == bmdDuplexInactive)
        return true;

    if (haveBusyState &&
        (busyState & bmdDeviceCaptureBusy) != 0)
    {
        // Be conservative if an older device/driver cannot report duplex:
        // capture-busy then means "do not offer this for TX".
        if (!haveDuplex || duplex != bmdDuplexFull)
            return true;
    }

    return false;
}


class ScheduledOutputCallback final : public IDeckLinkVideoOutputCallback
{
public:
    explicit ScheduledOutputCallback(AsioCapture* audio)
        : audio_(audio)
    {
    }

    struct Event
    {
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point hostTime{};
        std::int64_t qpcTicks = 0;
        BMDOutputFrameCompletionResult result = bmdOutputFrameCompleted;
        double asrcBufferFillMs = 0.0;
    };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;

        *ppv = nullptr;

        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, IID_IDeckLinkVideoOutputCallback))
        {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --refCount_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
        IDeckLinkVideoFrame* /*completedFrame*/,
        BMDOutputFrameCompletionResult result) override
    {
        Event event;
        {
            std::lock_guard lock(mutex_);
            event.sequence = ++sequence_;
            event.hostTime = std::chrono::steady_clock::now();
            LARGE_INTEGER qpc{};
            if (QueryPerformanceCounter(&qpc))
                event.qpcTicks = qpc.QuadPart;
            event.result = result;

            // 0.4.93: sample ASRC FIFO fill at the ACTUAL BMD callback instant.
            // The frame builder stops draining once its 40 ms payload is complete,
            // so our consumer is idle until this callback. The read is lock-free
            // via atomically published queue depth.
            if (audio_ != nullptr)
                event.asrcBufferFillMs = audio_->bmdBufferFillMsSnapshot();

            events_.push_back(event);

            // More than enough history for a worker that consumes callbacks
            // sequentially, while still surviving temporary scheduling stalls.
            while (events_.size() > 16)
                events_.pop_front();
        }

        condition_.notify_all();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
    {
        {
            std::lock_guard lock(mutex_);
            playbackStopped_ = true;
        }
        condition_.notify_all();
        return S_OK;
    }

    std::uint64_t sequence() const
    {
        std::lock_guard lock(mutex_);
        return sequence_;
    }

    bool waitForNext(
        std::uint64_t afterSequence,
        std::atomic<bool>& run,
        Event& event)
    {
        std::unique_lock lock(mutex_);

        const auto findEvent = [&]() -> bool
        {
            for (const Event& candidate : events_)
            {
                if (candidate.sequence > afterSequence)
                {
                    event = candidate;
                    return true;
                }
            }
            return false;
        };

        while (run.load())
        {
            if (findEvent())
                return true;

            if (playbackStopped_)
                return false;

            condition_.wait_for(lock, std::chrono::milliseconds(20));
        }

        return findEvent();
    }

private:
    AsioCapture* audio_ = nullptr; // non-owning; output worker owns lifetime ordering
    std::atomic<ULONG> refCount_{1};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Event> events_;
    std::uint64_t sequence_ = 0;
    bool playbackStopped_ = false;
};
}

DeckLinkPalOutput::DeckLinkPalOutput() = default;

DeckLinkPalOutput::~DeckLinkPalOutput()
{
    stop();
}

std::vector<DeckLinkDeviceInfo> DeckLinkPalOutput::enumerate()
{
    std::vector<DeckLinkDeviceInfo> result;

    const HRESULT coHr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coHr);

    IDeckLinkIterator* iterator = nullptr;

    if (SUCCEEDED(
            CoCreateInstance(
                CLSID_CDeckLinkIterator,
                nullptr,
                CLSCTX_ALL,
                IID_IDeckLinkIterator,
                reinterpret_cast<void**>(&iterator))))
    {
        IDeckLink* device = nullptr;
        int index = 0;

        while (iterator->Next(&device) == S_OK)
        {
            IDeckLinkOutput* output = nullptr;

            if (SUCCEEDED(
                    device->QueryInterface(
                        IID_IDeckLinkOutput,
                        reinterpret_cast<void**>(&output))))
            {
                result.push_back(
                    {
                        index,
                        nameOf(device),
                        playbackBusy(device)
                    });

                output->Release();
            }

            device->Release();
            ++index;
        }

        iterator->Release();
    }

    // Do not call CoUninitialize after RPC_E_CHANGED_MODE on Qt's GUI thread.
    if (uninitializeCom)
        CoUninitialize();

    return result;
}

bool DeckLinkPalOutput::start(
    int deviceIndex,
    AsioCapture* audio,
    Pcm16VideoEncoder* encoder,
    HamPcmV2Encoder* hamEncoder,
    bool hamMode)
{
    stop();

    {
        std::lock_guard lock(stateMutex_);
        error_.clear();
        audio_ = audio;
        encoder_ = encoder;
        hamEncoder_ = hamEncoder;
        hamMode_.store(hamMode);
    }

    if (hamMode)
    {
        if (hamEncoder != nullptr)
            hamEncoder->reset();
    }
    else if (encoder != nullptr)
    {
        // Proven 0.2.21 Sony/EIAJ reset path.
        encoder->reset();
    }

    frames_ = 0;
    worstJitMarginMs_ = 1.0e9;
    bmdSubmitFailures_ = 0;
    bmdScheduledCallbacks_ = 0;
    frameReadyCount_ = 0;
    bmdDisplayedLate_ = 0;
    bmdDroppedFrames_ = 0;
    bmdFlushedFrames_ = 0;
    jitMissedBoundaries_ = 0;
    run_ = true;
    thread_ =
        std::thread(
            &DeckLinkPalOutput::worker,
            this,
            deviceIndex);

    return true;
}

void DeckLinkPalOutput::stop()
{
    run_ = false;

    // The output worker may be blocked waiting for the next ASIO block.
    // Wake it so shutdown never depends on a timeout/polling interval.
    {
        std::lock_guard lock(stateMutex_);
        if (audio_ != nullptr)
            audio_->wakeAudioWaiters();
    }

    if (thread_.joinable())
        thread_.join();

    std::lock_guard lock(stateMutex_);
    audio_ = nullptr;
    encoder_ = nullptr;
    hamEncoder_ = nullptr;
    hamMode_.store(false);
}

void DeckLinkPalOutput::setError(const std::wstring& error)
{
    std::lock_guard lock(stateMutex_);
    error_ = error;
}

std::wstring DeckLinkPalOutput::lastError() const
{
    std::lock_guard lock(stateMutex_);
    return error_;
}

void DeckLinkPalOutput::worker(int wanted)
{
    RealtimeThreadPolicy realtimePolicy(L"Pro Audio");

    const HRESULT coHr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coHr);

    IDeckLinkIterator* iterator = nullptr;
    IDeckLink* device = nullptr;
    IDeckLinkOutput* output = nullptr;

    HRESULT hr =
        CoCreateInstance(
            CLSID_CDeckLinkIterator,
            nullptr,
            CLSCTX_ALL,
            IID_IDeckLinkIterator,
            reinterpret_cast<void**>(&iterator));

    int index = 0;

    if (SUCCEEDED(hr))
    {
        while (iterator->Next(&device) == S_OK)
        {
            if (index++ == wanted)
                break;

            device->Release();
            device = nullptr;
        }

        if (device == nullptr)
            hr = E_FAIL;
    }

    if (SUCCEEDED(hr))
    {
        hr = device->QueryInterface(
            IID_IDeckLinkOutput,
            reinterpret_cast<void**>(&output));
    }

    if (SUCCEEDED(hr))
    {
        hr = output->EnableVideoOutput(
            bmdModePAL,
            bmdVideoOutputFlagDefault);
    }

    if (FAILED(hr))
    {
        setError(
            L"DeckLink PAL output init failed: 0x" +
            std::to_wstring(static_cast<unsigned long>(hr)));
        run_ = false;
    }

    AsioCapture* audio = nullptr;
    Pcm16VideoEncoder* encoder = nullptr;
    HamPcmV2Encoder* hamEncoder = nullptr;
    bool hamMode = false;

    {
        std::lock_guard lock(stateMutex_);
        audio = audio_;
        encoder = encoder_;
        hamEncoder = hamEncoder_;
        hamMode = hamMode_.load();
    }

    std::vector<std::uint8_t> bytes;
    std::vector<std::int16_t> frameSamples;
    std::vector<std::int16_t> startupDiscard;

    // The user-selected value is the desired JIT reserve measured from
    // "PCM raster calculation complete" to Blackmagic's NEXT
    // ScheduledFrameCompleted callback. The callback is the real DeckLink
    // pacing event. The buffer measurement never assumes a 40 ms frame duration.
    while (run_ && SUCCEEDED(hr) && audio != nullptr && !audio->readyForPalFrame())
    {
        const auto sequence = audio->audioDataSequence();

        // Re-check after taking the sequence so an ASIO block arriving in
        // between cannot be missed.
        if (!audio->readyForPalFrame())
            audio->waitForAudioData(sequence);
    }

    constexpr auto FallbackFramePeriod = std::chrono::milliseconds(40);

    // Scheduled playback establishes the DeckLink cadence. No guessed
    // hardware-boundary wait is required here.

    if (audio != nullptr)
    {
        const long long targetReserveFramesLL = std::max<long long>(
            0LL,
            std::llround(audio->outputSampleRate() *
                         static_cast<double>(audio->bufferMs()) / 1000.0));
        const std::size_t targetReserveFrames =
            static_cast<std::size_t>(targetReserveFramesLL);
        const std::size_t queued = audio->queuedFrames();
        if (queued > targetReserveFrames)
        {
            startupDiscard.clear();
            const std::size_t excess = queued - targetReserveFrames;
            if (hamMode)
                audio->drainHamStereoFrames(startupDiscard, excess);
            else
                audio->drainStereoFrames(startupDiscard, excess);
        }
    }

    ScheduledOutputCallback* scheduledCallback =
        new ScheduledOutputCallback(audio);

    hr = output->SetScheduledFrameCompletionCallback(scheduledCallback);

    BMDTimeValue frameDuration = 1000;
    BMDTimeScale frameTimeScale = 25000;

    // Read the timing from the actual DeckLink display mode. PAL currently
    // falls back to 1000/25000 only if the mode iterator cannot provide it.
    if (SUCCEEDED(hr))
    {
        IDeckLinkDisplayModeIterator* modeIterator = nullptr;
        if (SUCCEEDED(output->GetDisplayModeIterator(&modeIterator)) &&
            modeIterator != nullptr)
        {
            IDeckLinkDisplayMode* mode = nullptr;
            while (modeIterator->Next(&mode) == S_OK)
            {
                if (mode->GetDisplayMode() == bmdModePAL)
                {
                    BMDTimeValue duration = 0;
                    BMDTimeScale scale = 0;
                    if (SUCCEEDED(mode->GetFrameRate(&duration, &scale)) &&
                        duration > 0 && scale > 0)
                    {
                        frameDuration = duration;
                        frameTimeScale = scale;
                    }

                    mode->Release();
                    break;
                }

                mode->Release();
            }

            modeIterator->Release();
        }
    }

    BMDTimeValue nextDisplayTime = 0;
    bool scheduledPlaybackStarted = false;
    bool haveLastCallback = false;
    bool haveFirstCallbackHost = false;
    std::chrono::steady_clock::time_point firstCallbackHost{};
    std::uint64_t bmdCallbackOrdinal = 0;
    auto lastCallbackHost = std::chrono::steady_clock::now();

    // Helper: create a DeckLink-owned frame and copy the already-calculated
    // PCM raster into it.
    auto createDeckFrame =
        [&](const std::vector<std::uint8_t>& raster,
            IDeckLinkMutableVideoFrame** outFrame) -> HRESULT
    {
        if (outFrame == nullptr)
            return E_POINTER;

        *outFrame = nullptr;

        HRESULT localHr = output->CreateVideoFrame(
            720,
            576,
            1440,
            bmdFormat8BitYUV,
            bmdFrameFlagDefault,
            outFrame);

        if (FAILED(localHr) || *outFrame == nullptr)
            return FAILED(localHr) ? localHr : E_FAIL;

        IDeckLinkVideoBuffer* buffer = nullptr;
        void* pixels = nullptr;

        localHr = (*outFrame)->QueryInterface(
            IID_IDeckLinkVideoBuffer,
            reinterpret_cast<void**>(&buffer));

        if (SUCCEEDED(localHr) && buffer != nullptr)
            localHr = buffer->StartAccess(bmdBufferAccessWrite);

        if (SUCCEEDED(localHr))
        {
            localHr = buffer->GetBytes(&pixels);
            if (SUCCEEDED(localHr) && pixels != nullptr)
                std::memcpy(pixels, raster.data(), raster.size());
            else
                localHr = E_FAIL;

            buffer->EndAccess(bmdBufferAccessWrite);
        }

        if (buffer != nullptr)
            buffer->Release();

        if (FAILED(localHr))
        {
            (*outFrame)->Release();
            *outFrame = nullptr;
        }

        return localHr;
    };

    // Build and schedule the first frame as preroll. Buffer tracking starts on the
    // next frame, using the completion callback of this seed frame as its first
    // real Blackmagic timing event.
    if (SUCCEEDED(hr) && run_ && audio != nullptr &&
        hamEncoder != nullptr && encoder != nullptr)
    {
        const std::size_t seedRequiredFrames = hamMode
            ? static_cast<std::size_t>(HamPcmV2Encoder::SamplesPerFrame)
            : static_cast<std::size_t>(Pcm16VideoEncoder::SamplesPerFrame);

        frameSamples.clear();
        frameSamples.reserve(seedRequiredFrames * 2);

        while (run_ && frameSamples.size() / 2 < seedRequiredFrames)
        {
            const std::size_t remainingFrames =
                seedRequiredFrames - frameSamples.size() / 2;

            const std::size_t drained = hamMode
                ? audio->drainHamStereoFrames(frameSamples, remainingFrames)
                : audio->drainStereoFrames(frameSamples, remainingFrames);

            if (drained == 0)
            {
                const auto sequence = audio->audioDataSequence();

                // Re-drain once after taking the sequence. This closes the
                // race where ASIO supplied a block between drain==0 and wait.
                const std::size_t retryRemaining =
                    seedRequiredFrames - frameSamples.size() / 2;
                const std::size_t retryDrained = hamMode
                    ? audio->drainHamStereoFrames(frameSamples, retryRemaining)
                    : audio->drainStereoFrames(frameSamples, retryRemaining);

                if (retryDrained == 0 && run_)
                    audio->waitForAudioData(sequence);
            }
        }

        if (run_)
        {
            if (hamMode)
                hamEncoder->encodeFrame(frameSamples, bytes);
            else
                encoder->encodeFrame(frameSamples, bytes);

            IDeckLinkMutableVideoFrame* seedFrame = nullptr;
            hr = createDeckFrame(bytes, &seedFrame);

            if (SUCCEEDED(hr))
            {
                hr = output->ScheduleVideoFrame(
                    seedFrame,
                    nextDisplayTime,
                    frameDuration,
                    frameTimeScale);

                seedFrame->Release();

                if (SUCCEEDED(hr))
                {
                    ++frames_;
                    nextDisplayTime += frameDuration;

                    hr = output->StartScheduledPlayback(
                        0,
                        frameTimeScale,
                        1.0);

                    if (SUCCEEDED(hr))
                        scheduledPlaybackStarted = true;
                }
                else
                {
                    ++bmdSubmitFailures_;
                }
            }
        }
    }

    while (run_ && SUCCEEDED(hr) && audio != nullptr &&
           hamEncoder != nullptr && encoder != nullptr)
    {
        // Snapshot the current Blackmagic cadence sequence BEFORE we start
        // collecting/building this frame.
        //
        // If the intended BMD callback occurs while this frame is still being
        // calculated, that callback will already be queued by the time we are
        // ready. waitForNext() will then return it immediately instead of
        // incorrectly waiting for another whole video frame.
        const std::uint64_t frameBoundarySequence =
            scheduledCallback->sequence();
        // Snapshot encoder family at the start of this PAL frame so a live UI
        // switch can never produce a mixed-format frame.
        const bool requestedHamMode = hamMode_.load();
        if (requestedHamMode != hamMode)
        {
            hamMode = requestedHamMode;
            audio->setHamMode(hamMode);
            if (hamMode)
                hamEncoder->reset();
            else
                encoder->reset();
        }

        const std::size_t requiredFrames = hamMode
            ? static_cast<std::size_t>(HamPcmV2Encoder::SamplesPerFrame)
            : static_cast<std::size_t>(Pcm16VideoEncoder::SamplesPerFrame);

        // Continuously drain post-ASRC samples into the current frame's audio
        // payload. At a 4 ms target there are about 4 ms ready at the boundary;
        // the rest arrives from ASIO/ASRC during this same 40 ms frame period.
        frameSamples.clear();
        frameSamples.reserve(requiredFrames * 2);

        while (run_ && frameSamples.size() / 2 < requiredFrames)
        {
            const std::size_t remainingFrames =
                requiredFrames - frameSamples.size() / 2;
            const std::size_t drained = hamMode
                ? audio->drainHamStereoFrames(frameSamples, remainingFrames)
                : audio->drainStereoFrames(frameSamples, remainingFrames);

            if (drained == 0)
            {
                const auto sequence = audio->audioDataSequence();

                // Re-drain once after taking the sequence. This closes the
                // race where ASIO supplied a block between drain==0 and wait.
                const std::size_t retryRemaining =
                    requiredFrames - frameSamples.size() / 2;
                const std::size_t retryDrained = hamMode
                    ? audio->drainHamStereoFrames(frameSamples, retryRemaining)
                    : audio->drainStereoFrames(frameSamples, retryRemaining);

                if (retryDrained == 0 && run_)
                    audio->waitForAudioData(sequence);
            }
        }

        if (!run_)
            break;

        // As soon as the last required sample exists, finish calculating the
        // PCM raster. THIS is "I AM READY".
        if (hamMode)
            hamEncoder->encodeFrame(frameSamples, bytes);
        else
            encoder->encodeFrame(frameSamples, bytes);

        const auto frameReadyHost = std::chrono::steady_clock::now();
        LARGE_INTEGER frameReadyQpc{};
        if (!QueryPerformanceCounter(&frameReadyQpc))
            frameReadyQpc.QuadPart = 0;
        ++frameReadyCount_;
        // The cadence sequence for this frame was captured at the START of
        // frame production, before any PCM collection/encode work.
        IDeckLinkMutableVideoFrame* frame = nullptr;
        hr = createDeckFrame(bytes, &frame);

        if (SUCCEEDED(hr))
        {
            hr = output->ScheduleVideoFrame(
                frame,
                nextDisplayTime,
                frameDuration,
                frameTimeScale);

            frame->Release();

            if (SUCCEEDED(hr))
            {
                ++frames_;
                nextDisplayTime += frameDuration;
            }
            else
            {
                ++bmdSubmitFailures_;
            }
        }

        if (FAILED(hr))
            break;        // Frame is complete and scheduled; consumer stays quiescent here until
        // the callback captures FIFO fill. No ASRC drain occurs in this interval.
        ScheduledOutputCallback::Event callbackEvent;
        if (!scheduledCallback->waitForNext(
                frameBoundarySequence,
                run_,
                callbackEvent))
        {
            break;
        }

        ++bmdScheduledCallbacks_;

        switch (callbackEvent.result)
        {
        case bmdOutputFrameDisplayedLate:
            ++bmdDisplayedLate_;
            break;

        case bmdOutputFrameDropped:
            ++bmdDroppedFrames_;
            break;

        case bmdOutputFrameFlushed:
            ++bmdFlushedFrames_;
            break;

        default:
            break;
        }

        // This is the buffer-fill measurement:
        //
        //     I AM READY  -------------------->  BMD callback
        //
        // If the callback arrived while the PC was still preparing/scheduling
        // the frame, the frame missed its JIT reserve. The margin itself is
        // defined as non-negative, so clamp that miss to 0 ms; the DeckLink
        // completion-result counters above show whether it was late/dropped.
        const double rawCallbackMarginMs =
            std::chrono::duration<double, std::milli>(
                callbackEvent.hostTime - frameReadyHost).count();

        // If the cadence callback already happened while we were still
        // calculating/scheduling the frame, we missed the desired reserve.
        // Clamp the control sample to 0 ms; crucially, do NOT wait for another
        // callback and therefore do NOT introduce a +one-frame phase jump.
        const double callbackMarginMs =
            std::max(0.0, rawCallbackMarginMs);

        if (rawCallbackMarginMs < 0.0)
            ++jitMissedBoundaries_;

        const double sampleDtSec = haveLastCallback
            ? std::max(
                0.001,
                std::chrono::duration<double>(
                    callbackEvent.hostTime - lastCallbackHost).count())
            : static_cast<double>(frameDuration) /
              static_cast<double>(frameTimeScale);

        if (!haveFirstCallbackHost)
        {
            firstCallbackHost = callbackEvent.hostTime;
            haveFirstCallbackHost = true;
            bmdCallbackOrdinal = 0;
        }

        const double bmdCallbackElapsedMs =
            std::chrono::duration<double, std::milli>(
                callbackEvent.hostTime - firstCallbackHost).count();
        const double bmdFramePeriodMs =
            1000.0 * static_cast<double>(frameDuration) /
            static_cast<double>(frameTimeScale);
        const double bmdIdealElapsedMs =
            static_cast<double>(bmdCallbackOrdinal) * bmdFramePeriodMs;
        ++bmdCallbackOrdinal;

        lastCallbackHost = callbackEvent.hostTime;
        haveLastCallback = true;

        double observedWorst = worstJitMarginMs_.load();
        while (callbackMarginMs < observedWorst &&
               !worstJitMarginMs_.compare_exchange_weak(
                   observedWorst, callbackMarginMs))
        {
        }

        // 0.4.93: by the time we arrive here, frameSamples was completed before
        // the callback and the ASRC consumer has remained idle ever since.
        // Use the FIFO fill captured at the actual ScheduledFrameCompleted callback.
        audio->updateBufferNudge(
            callbackEvent.asrcBufferFillMs,
            callbackMarginMs,
            sampleDtSec,
            bmdCallbackElapsedMs,
            bmdIdealElapsedMs,
            frameReadyQpc.QuadPart,
            callbackEvent.qpcTicks);
    }

    if (output != nullptr)
    {
        if (scheduledPlaybackStarted)
        {
            BMDTimeValue actualStopTime = 0;
            output->StopScheduledPlayback(
                0,
                &actualStopTime,
                frameTimeScale);
        }

        output->SetScheduledFrameCompletionCallback(nullptr);

        if (scheduledCallback != nullptr)
        {
            scheduledCallback->Release();
            scheduledCallback = nullptr;
        }

        output->DisableVideoOutput();
        output->Release();
    }

    if (device != nullptr)
        device->Release();

    if (iterator != nullptr)
        iterator->Release();

    if (FAILED(hr) && lastError().empty())
    {
        setError(
            L"DeckLink output stopped: 0x" +
            std::to_wstring(static_cast<unsigned long>(hr)));
    }

    run_ = false;

    if (uninitializeCom)
        CoUninitialize();
}
