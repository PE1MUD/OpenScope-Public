#pragma once

#include "video/Yuv444Frame.h"

#include <cstdint>
#include <string>

class WssDecoder
{
public:
    enum class Format
    {
        None = -1,
        Full4x3 = 0,
        Letterbox14x9Centre = 1,
        Letterbox14x9Top = 2,
        Letterbox16x9Centre = 3,
        Letterbox16x9Top = 4,
        LetterboxWiderCentre = 5,
        Full4x3Protect14x9 = 6,
        Full16x9Anamorphic = 7
    };

    struct Result
    {
        bool validThisFrame = false;
        bool locked = false;
        bool stateChanged = false;
        int detectedLine = -1;
        int displayBlankLine = -1;
        Format format = Format::None;
        int recommendedDisplayAspect = -1; // -1 none, 0=4:3, 1=16:9
        std::string status;
    };

    Result process(const Yuv444Frame& frame);

    static bool injectTestSignal(
        Yuv444Frame& frame,
        Format format,
        int lineIndex = 0);

private:
    struct Candidate
    {
        bool valid = false;
        int line = -1;
        int formatCode = -1;
        double score = 0.0;
    };

    static Candidate decodeLine(
        const std::uint16_t* line,
        int width,
        double sampleClockHz,
        int lineIndex);

    static std::string formatDescription(Format format);
    static int recommendedDisplayAspect(Format format);

    int pendingFormatCode_ = -1;
    int pendingCount_ = 0;
    int lockedFormatCode_ = -1;
    int lockedLine_ = -1;
    int missCount_ = 0;

    bool lastPublishedLocked_ = false;
    int lastPublishedFormatCode_ = -2;
    static constexpr int kStableFramesRequired = 3;
    static constexpr int kUnlockMissFrames = 8;
};
