#pragma once

#include "video/ProgressiveLumaPair.h"

#include <cstdint>
#include <cstddef>
#include <vector>

class VideoDeinterlacer
{
public:
    void deinterlace(
        const std::uint16_t* source,
        int width,
        int height,
        ProgressiveLumaPair& destination);

    bool beginFrame(
        const std::uint16_t* source,
        int width,
        int height,
        ProgressiveLumaPair& destination);

    void processRange(
        int firstLine,
        int lastLine);

    void endFrame();

private:
    enum class FrameMode
    {
        First,
        Second,
        Normal
    };

    std::vector<std::uint16_t> previousLuma_;
    std::vector<std::uint16_t> previousPreviousLuma_;
    std::vector<std::uint8_t> motionMask_;

    bool hasPreviousFrame_ = false;

    const std::uint16_t* currentSource_ = nullptr;
    ProgressiveLumaPair* currentDestination_ = nullptr;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    std::size_t currentSampleCount_ = 0;
    FrameMode currentMode_ = FrameMode::First;
};