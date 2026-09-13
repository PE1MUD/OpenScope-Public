#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WasapiPcmOutput.h"

class HamWasapiPcmOutput final
{
public:
    HamWasapiPcmOutput();
    ~HamWasapiPcmOutput();

    HamWasapiPcmOutput(const HamWasapiPcmOutput&) = delete;
    HamWasapiPcmOutput& operator=(const HamWasapiPcmOutput&) = delete;

    static std::vector<WasapiOutputDevice> enumerateDevices();

    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Empty ID means the current Windows default render endpoint.
    void setDeviceId(const std::string& endpointIdUtf8);
    std::string deviceId() const;

    void setBufferDepthMs(int milliseconds);
    int bufferDepthMs() const noexcept { return bufferDepthMs_.load(std::memory_order_acquire); }

    void pushStereo48000(const std::vector<std::int16_t>& interleavedStereo);
    void clear();

private:
    void workerLoop();

    std::atomic_bool enabled_{ false };
    std::atomic_bool stop_{ false };
    std::atomic<std::uint64_t> deviceGeneration_{ 0 };
    std::atomic<int> bufferDepthMs_{ 32 };
    std::thread thread_;

    mutable std::mutex deviceMutex_;
    std::string deviceIdUtf8_;

    std::mutex mutex_;
    std::deque<std::int16_t> fifo_;
    double resamplePhase_ = 0.0;
    double filteredQueuedFrames_ = 2880.0;
};
