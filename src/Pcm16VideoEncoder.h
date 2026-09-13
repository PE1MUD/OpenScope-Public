#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class Pcm16VideoEncoder {
public:
    static constexpr int Width=720, Height=576, SamplesPerFrame=1764;

    void reset();
    void encodeFrame(const std::vector<std::int16_t>& stereo, std::vector<std::uint8_t>& uyvy);

    void setPulseShapingEnabled(bool enabled) { pulseShapingEnabled_.store(enabled); }
    void setVideoBandwidthMHz(double mhz) { videoBandwidthMHz_.store(mhz); }

    bool pulseShapingEnabled() const { return pulseShapingEnabled_.load(); }
    double videoBandwidthMHz() const { return videoBandwidthMHz_.load(); }

    std::uint64_t frameCount() const { return frame_; }
private:
    using Group=std::array<std::uint16_t,7>; // A0 B0 A1 B1 A2 B2 P
    Group makeGroup(const std::int16_t* s) const;
    Group groupAt(std::int64_t g) const;
    std::uint16_t qslot(std::int64_t g) const;
    std::array<std::uint16_t,8> physicalWords(std::int64_t g) const;
    static std::uint16_t crc(const std::array<std::uint16_t,8>& w);
    void renderLine(std::uint8_t* uyvyRow,const std::array<std::uint16_t,8>& w) const;
    static void shapeLuma(std::array<float, Width>& y, double bandwidthMHz);

    std::atomic<bool> pulseShapingEnabled_{true};
    std::atomic<double> videoBandwidthMHz_{3.5};

    std::vector<Group> history_;
    std::int64_t firstGroup_=0;
    std::uint64_t frame_=0;
};
