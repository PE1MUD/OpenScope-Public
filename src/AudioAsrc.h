#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class AudioAsrc
{
public:
    static constexpr double OutputRate = 44100.0;

    void reset(double inputRate);
    void pushInterleavedFloat(const float* stereo, std::size_t frames);

    // Always returns exactly requestedFrames stereo frames. If input is
    // temporarily insufficient, the tail is filled with silence.
    std::vector<std::int16_t> takeInt16(std::size_t requestedFrames);

    std::size_t queuedInputFrames() const { return fifo_.size() / 2; }
    static constexpr std::size_t targetInputFrames() { return TargetFrames; }
    double inputRate() const { return inputRate_; }
    double correctionPpm() const { return correctionPpm_; }
    std::uint64_t underruns() const { return underruns_; }

private:
    float interpolate(int channel, double position) const;
    void trimConsumed();

    static constexpr int HalfTaps = 16; // 32-tap windowed-sinc
    // The DeckLink PAL consumer asks for 1764 output frames at once
    // (44100 / 25). At a 48 kHz input rate that consumes about 1920 input
    // frames, plus FIR look-ahead. Keep comfortably more than one complete
    // PAL-frame conversion in the elastic FIFO.
    static constexpr std::size_t TargetFrames = 3072;
    static constexpr double MaxCorrectionPpm = 500.0;

    std::deque<float> fifo_; // interleaved stereo
    double inputRate_ = 48000.0;
    double sourcePos_ = HalfTaps;
    double correctionPpm_ = 0.0;
    double filteredError_ = 0.0;
    std::uint64_t underruns_ = 0;
};
