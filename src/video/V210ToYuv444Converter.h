#pragma once

#include "Video/Yuv444Frame.h"
#include "Video/VideoConverter.h"
#include <cstdint>

class V210ToYuv444Converter final : public VideoConverter
{
public:
    bool convert(
        const std::uint8_t* source,
        int rowBytes,
        int width,
        int height,
        Yuv444Frame& destination) const override;
};