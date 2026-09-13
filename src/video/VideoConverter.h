#pragma once

#include "Video/Yuv444Frame.h"

#include <cstdint>

class VideoConverter
{
public:
    virtual ~VideoConverter() = default;

    virtual bool convert(
        const std::uint8_t* source,
        int rowBytes,
        int width,
        int height,
        Yuv444Frame& destination) const = 0;
};