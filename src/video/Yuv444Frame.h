#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Yuv444Frame
{
#pragma once

#include <cstdint>
#include <vector>

    struct ReconstructedLumaFrame
    {
        int width = 0;
        int height = 0;

        std::vector<std::uint16_t> y;

        void resize(int newWidth, int newHeight)
        {
            width = newWidth;
            height = newHeight;

            y.resize(
                static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height));
        }
    };
    int width = 0;
    int height = 0;

    // Horizontal sample clock of the source raster.
    // Blackmagic PAL/NTSC D1 uses 13.5 MHz; some Philips ROM sets use 20 MHz.
    double sampleClockHz = 13'500'000.0;

    // Source/capture validity metadata. Sources are valid by default;
    // hardware capture may explicitly mark a delivered frame invalid.
    bool inputSignalValid = true;

    // Each plane contains width * height samples.
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> u;
    std::vector<std::uint16_t> v;

    void resize(int newWidth, int newHeight)
    {
        width = newWidth;
        height = newHeight;

        const std::size_t sampleCount =
            static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height);

        y.resize(sampleCount);
        u.resize(sampleCount);
        v.resize(sampleCount);
    }
};