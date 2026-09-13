#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DeckLinkDeviceInfo
{
    int index = 0;
    std::wstring name;
    bool busy = false;
};

class AsioCapture;
class Pcm16VideoEncoder;
class HamPcmV2Encoder;

class DeckLinkPalOutput
{
public:
    DeckLinkPalOutput();
    ~DeckLinkPalOutput();

    static std::vector<DeckLinkDeviceInfo> enumerate();

    bool start(
        int deviceIndex,
        AsioCapture* audio,
        Pcm16VideoEncoder* sonyEncoder,
        HamPcmV2Encoder* hamEncoder,
        bool hamMode);

    void stop();
    void setHamMode(bool enabled) { hamMode_.store(enabled); }

    bool running() const { return run_.load(); }
    std::wstring lastError() const;
    std::uint64_t frames() const { return frames_.load(); }
    double worstJitMarginMs() const { return worstJitMarginMs_.load(); }
    std::uint64_t bmdSubmitFailures() const { return bmdSubmitFailures_.load(); }
    std::uint64_t bmdScheduledCallbacks() const { return bmdScheduledCallbacks_.load(); }
    std::uint64_t frameReadyCount() const { return frameReadyCount_.load(); }
    std::uint64_t bmdDisplayedLate() const { return bmdDisplayedLate_.load(); }
    std::uint64_t bmdDroppedFrames() const { return bmdDroppedFrames_.load(); }
    std::uint64_t bmdFlushedFrames() const { return bmdFlushedFrames_.load(); }
    std::uint64_t jitMissedBoundaries() const { return jitMissedBoundaries_.load(); }

private:
    void worker(int deviceIndex);
    void setError(const std::wstring& error);

    std::atomic<bool> run_{false};
    std::thread thread_;

    mutable std::mutex stateMutex_;
    std::wstring error_;

    AsioCapture* audio_ = nullptr;
    Pcm16VideoEncoder* encoder_ = nullptr;
    HamPcmV2Encoder* hamEncoder_ = nullptr;
    std::atomic_bool hamMode_{false};

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<double> worstJitMarginMs_{1.0e9};
    std::atomic<std::uint64_t> bmdSubmitFailures_{0};
    std::atomic<std::uint64_t> bmdScheduledCallbacks_{0};
    std::atomic<std::uint64_t> frameReadyCount_{0};
    std::atomic<std::uint64_t> bmdDisplayedLate_{0};
    std::atomic<std::uint64_t> bmdDroppedFrames_{0};
    std::atomic<std::uint64_t> bmdFlushedFrames_{0};
    std::atomic<std::uint64_t> jitMissedBoundaries_{0};
};
