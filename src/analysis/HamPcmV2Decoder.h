#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class HamPcmV2Decoder final
{
public:
    struct Result
    {
        bool locked = false;
        int validLines = 0;
        int testedLines = 0;
        double crcPercent = 0.0;
        double bitPeriodPixels = 0.0;
        double syncStartPixels = 0.0;
        int field1ValidAudioLines = 0;
        int field1TestedAudioLines = 0;
        int field2ValidAudioLines = 0;
        int field2TestedAudioLines = 0;
        int field1AudioPairs = 0;
        int field2AudioPairs = 0;
        std::uint64_t correctedRows = 0;
        std::uint64_t refinementFallbackRows = 0;
        std::uint64_t hardUncorrectableRows = 0;
        std::string text;
        std::vector<std::int16_t> audioStereo;
    };

    Result processLuma(
        const std::vector<std::uint16_t>& y,
        int width,
        int height,
        bool inputSignalValid);

    // Cheap format-presence probe for Auto mode. It samples only two rows and
    // never changes decoder lock/tracking state.
    bool probeLuma(
        const std::vector<std::uint16_t>& y,
        int width,
        int height,
        bool inputSignalValid) const;

    void reset();

private:
    bool textCollecting_ = false;
    int textRows_ = 0;
    std::uint64_t textBits_ = 0;
    std::string text_;
    bool geometryHintValid_ = false;
    double geometryStartHint_ = 12.0;
    double geometryPeriodHint_ = 696.0 / 180.0;
};
