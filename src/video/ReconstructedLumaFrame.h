#pragma once

#include <cstdint>
#include <vector>

struct ReconstructedLumaFrame
{
    std::uint64_t generation = 0;

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