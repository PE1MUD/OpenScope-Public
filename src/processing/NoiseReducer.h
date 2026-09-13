#pragma once

#include "video/Yuv444Frame.h"

#include <cstdint>
#include <vector>

class NoiseReducer
{
public:
    void process(
        const Yuv444Frame& source,
        Yuv444Frame& destination,
        int intensity);

    // Thread-friendly range entry point. The caller must size destination
    // before dispatching workers. Each worker writes only [firstLine,lastLine)
    // and may therefore run in parallel with another non-overlapping range.
    void processRange(
        const Yuv444Frame& source,
        Yuv444Frame& destination,
        int intensity,
        int firstLine,
        int lastLine) const;

private:
    static constexpr int kMinimumIntensity = 0;
    static constexpr int kMaximumIntensity = 100;

    // Intensity controls both edge acceptance and blend.
    // 0 keeps the original image.
    // 50 matches the earlier conservative filter.
    // 100 deliberately becomes much more aggressive.
    static constexpr std::uint16_t kMinimumLumaThreshold = 1536;
    static constexpr std::uint16_t kMaximumLumaThreshold = 6144;

    static constexpr std::uint16_t kMinimumChromaThreshold = 2304;
    static constexpr std::uint16_t kMaximumChromaThreshold = 9216;

    static constexpr int kFilterRadius = 2;

    static void filterPlaneRange(
        const std::vector<std::uint16_t>& source,
        std::vector<std::uint16_t>& destination,
        int width,
        int height,
        std::uint16_t threshold,
        int firstLine,
        int lastLine);

    static void blendPlaneRange(
        const std::vector<std::uint16_t>& source,
        std::vector<std::uint16_t>& filtered,
        int width,
        int height,
        int intensity,
        int firstLine,
        int lastLine);
};
