#pragma once

#include "Yuv444Frame.h"
#include "Video/VideoConverter.h"
#include <cstdint>

class Uyvy422ToYuv444Converter final : public VideoConverter
{
public:
    bool convert(
        const std::uint8_t* source,
        int rowBytes,
        int width,
        int height,
        Yuv444Frame& destination) const override;

private:
    static constexpr std::uint16_t expand8To16(std::uint8_t value) noexcept
    {
        // Exact mapping:
        //   0   -> 0
        //   255 -> 65535
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(value) * 257u);
    }

    static constexpr std::uint16_t interpolate(
        std::uint8_t left,
        std::uint8_t right) noexcept
    {
        // Average in the 8-bit domain with controlled rounding,
        // then expand exactly to the 16-bit internal range.
        const unsigned average =
            (static_cast<unsigned>(left) +
             static_cast<unsigned>(right) + 1u) >> 1;

        return static_cast<std::uint16_t>(average * 257u);
    }
};