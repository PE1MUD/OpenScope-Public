#pragma once

#include "AudioAsrc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioInputDevice
{
    std::wstring id;
    std::wstring name;
};

class WasapiCapture
{
public:
    WasapiCapture();
    ~WasapiCapture();

    static std::vector<AudioInputDevice> enumerate();

    bool start(const std::wstring& deviceId);
    void stop();

    // Consumer side is always fixed 44.1 kHz stereo PCM16.
    std::vector<std::int16_t> takeStereoFrames(std::size_t frames);

    float peakLeft() const { return peakL_.load(); }
    float peakRight() const { return peakR_.load(); }

    std::size_t queuedFrames() const;

    // Enough native-rate input for one complete 1764-frame PAL audio block,
    // including FIR look-ahead.
    bool readyForPalFrame() const;
    std::uint64_t underruns() const;
    double inputSampleRate() const { return inputRate_.load(); }
    double asrcCorrectionPpm() const { return asrcPpm_.load(); }

    std::wstring lastError() const;

private:
    void worker(std::wstring deviceId);

    std::atomic<bool> run_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    AudioAsrc asrc_;
    std::wstring error_;

    std::atomic<float> peakL_{0.0f};
    std::atomic<float> peakR_{0.0f};
    std::atomic<double> inputRate_{0.0};
    std::atomic<double> asrcPpm_{0.0};
};
