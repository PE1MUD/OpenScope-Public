#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <Unknwn.h>

#ifndef interface
#define interface struct
#endif

#include <asio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AsioOutputDriver
{
    std::string nameUtf8;
};

class BufferNudgeController;

class AsioPcmOutput final
{
public:
    AsioPcmOutput();
    ~AsioPcmOutput();

    AsioPcmOutput(const AsioPcmOutput&) = delete;
    AsioPcmOutput& operator=(const AsioPcmOutput&) = delete;

    static std::vector<AsioOutputDriver> enumerateDrivers();

    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    void setDriverName(const std::string& driverNameUtf8);
    std::string driverName() const;

    void pushStereo(const std::vector<std::int16_t>& interleavedStereo, double sourceSampleRate);
    void clear();

    long bufferFrames() const noexcept { return bufferFrames_.load(std::memory_order_acquire); }
    double bufferPeriodMs() const noexcept { return bufferPeriodMs_.load(std::memory_order_acquire); }
    int defaultTargetMs() const noexcept { return defaultTargetMs_.load(std::memory_order_acquire); }
    double queuedMs() const noexcept { return queuedMs_.load(std::memory_order_acquire); }
    double lowWaterMs() const noexcept { return lowWaterMs_.load(std::memory_order_acquire); }
    double nudgePpm() const noexcept { return nudgePpm_.load(std::memory_order_acquire); }
    std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_acquire); }
    std::uint64_t overruns() const noexcept { return overruns_.load(std::memory_order_acquire); }
    std::uint64_t callbackCount() const noexcept { return callbackCount_.load(std::memory_order_acquire); }
    double targetMs() const noexcept;
    double fastFillMs() const noexcept { return fastFillAtomic_.load(std::memory_order_acquire); }
    double average3sFillMs() const noexcept { return avg3FillAtomic_.load(std::memory_order_acquire); }
    double asioSampleRate() const noexcept { return asioRateAtomic_.load(std::memory_order_acquire); }
    void setTargetMs(double milliseconds); // <= 0 means automatic 4Q

    void setBufferLoggingEnabled(bool enabled);
    bool bufferLoggingEnabled() const noexcept { return bufferLoggingEnabled_.load(std::memory_order_acquire); }

private:
    void workerLoop();
    bool openDriver(const std::string& name, double requestedRate);
    void closeDriver();
    void requestRestart();

    static void bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
    static void sampleRateDidChange(ASIOSampleRate sRate);
    static long asioMessages(long selector, long value, void* message, double* opt);

    void render(long doubleBufferIndex);
    void writeSample(void* destination, ASIOSampleType type, double value) const;

    static std::atomic<AsioPcmOutput*> activeInstance_;

    std::atomic_bool enabled_{ false };
    std::atomic_bool stop_{ false };
    std::atomic_bool restartRequested_{ false };
    std::thread thread_;
    std::condition_variable condition_;
    std::mutex conditionMutex_;

    mutable std::mutex configMutex_;
    std::string driverNameUtf8_;
    double requestedSourceRate_ = 0.0;

    std::mutex fifoMutex_;
    std::deque<std::int16_t> fifo_;
    double sourceRate_ = 0.0;
    double sourcePhase_ = 0.0;
    std::chrono::steady_clock::time_point lastPushTime_{};

    ASIODriverInfo driverInfo_{};
    ASIOCallbacks callbacks_{};
    std::vector<ASIOBufferInfo> bufferInfos_;
    std::vector<ASIOChannelInfo> channelInfos_;
    bool driverLoaded_ = false;
    bool asioInitialized_ = false;
    bool buffersCreated_ = false;
    bool asioStarted_ = false;
    double asioRate_ = 0.0;

    std::unique_ptr<BufferNudgeController> nudgeController_;

    std::atomic<long> bufferFrames_{ 0 };
    std::atomic<double> bufferPeriodMs_{ 0.0 };
    std::atomic<int> defaultTargetMs_{ 0 };
    std::atomic<double> queuedMs_{ 0.0 };
    std::atomic<double> lowWaterMs_{ 0.0 };
    std::atomic<double> nudgePpm_{ 0.0 };
    std::atomic<double> targetOverrideMs_{ 0.0 };
    std::atomic<double> fastFillAtomic_{ 0.0 };
    std::atomic<double> avg3FillAtomic_{ 0.0 };
    std::atomic<double> asioRateAtomic_{ 0.0 };
    std::atomic<std::uint64_t> underruns_{ 0 };
    std::atomic<std::uint64_t> overruns_{ 0 };
    std::atomic<std::uint64_t> callbackCount_{ 0 };

    // Non-real-time diagnostics. Written only from pushStereo(), never from the ASIO callback.
    std::atomic_bool bufferLoggingEnabled_{ false };
    std::mutex logMutex_;
    std::ofstream bufferLog_;
    std::chrono::steady_clock::time_point logEpoch_{};
    std::chrono::steady_clock::time_point lastLog_{};
    std::uint64_t lastLoggedCallbacks_ = 0;
    std::uint64_t lastLoggedUnderruns_ = 0;
    std::uint64_t lastLoggedOverruns_ = 0;
    double fastFillMs_ = 0.0;
    double avg3FillMs_ = 0.0;
    bool fillAveragesValid_ = false;
};
