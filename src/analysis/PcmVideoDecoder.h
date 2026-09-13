#pragma once

#include "video/Yuv444Frame.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

class PcmVideoDecoder final
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
        int consecutiveLockedFrames = 0;

        bool modeKnown = false;
        bool mode16Detected = false;
        bool controlValid = false;
        bool preEmphasis = false;
        int controlValidLines = 0;
        int controlTestedLines = 0;
        int field1ValidAudioLines = 0;
        int field1TestedAudioLines = 0;
        int field1ValidControlLines = 0;
        int field1TestedControlLines = 0;
        int field2ValidAudioLines = 0;
        int field2TestedAudioLines = 0;
        int field2ValidControlLines = 0;
        int field2TestedControlLines = 0;
        std::uint64_t reconstructedGroups = 0;
        std::uint64_t pCorrectedGroups = 0;
        std::uint64_t lsbPackMissingGroups = 0;
        std::uint64_t hardUncorrectableGroups = 0;
        double pVerifyPercent = 0.0;
        double leftFrequencyHz = 0.0;
        double rightFrequencyHz = 0.0;
        std::vector<std::int16_t> audioStereo;
    };

    Result processLuma(
        const std::vector<std::uint16_t>& y,
        int width,
        int height,
        bool inputSignalValid,
        std::uint64_t captureGeneration,
        bool muteBottomTwoBits);

    void reset();

private:
    struct Geometry
    {
        double syncStart = 0.0;
        double bitPeriod = 0.0;
        bool valid = false;
    };

    struct PhysicalBlock
    {
        std::array<std::uint16_t, 8> words{};
    };

    static std::uint16_t thresholdForGeometry(
        const std::uint16_t* line,
        int width,
        const Geometry& geometry,
        bool& contrastOk);

    static bool sampleBit(
        const std::uint16_t* line,
        int width,
        double x,
        std::uint16_t threshold);

    static int structureScore(
        const std::uint16_t* line,
        int width,
        const Geometry& geometry,
        std::uint16_t threshold);

    static bool decodePayload(
        const std::uint16_t* line,
        int width,
        const Geometry& geometry,
        std::uint16_t threshold,
        std::uint8_t* payload128);

    static bool crcValid(
        const std::uint8_t* payload128);

    static PhysicalBlock payloadToWords(
        const std::uint8_t* payload128);

    Geometry searchGeometry(
        const std::vector<std::uint16_t>& y,
        int width,
        int height) const;

    int decodeFrameLines(
        const std::vector<std::uint16_t>& y,
        int width,
        int height,
        const Geometry& geometry,
        std::int64_t frameBaseLine,
        int& testedLines,
        int& controlValidLines,
        int& controlTestedLines,
        std::array<int, 2>& validAudioByField,
        std::array<int, 2>& testedAudioByField,
        std::array<int, 2>& validControlByField,
        std::array<int, 2>& testedControlByField);

    void reconstructAvailableGroups(
        std::int64_t newestPhysicalLine);

    bool reconstruct14BitGroup(
        std::int64_t group,
        std::array<std::uint16_t, 6>& audio,
        bool& corrected,
        bool& cleanPVerified);

    bool reconstruct16BitGroup(
        std::int64_t group,
        std::array<std::uint16_t, 6>& audio,
        bool& corrected,
        bool& cleanPVerified,
        bool& lsbPackMissing);

    void appendAudioGroup(
        std::int64_t group,
        const std::array<std::uint16_t, 6>& audio);

    static std::uint16_t gfMulX14(std::uint16_t v);
    static std::uint16_t gfMul14(std::uint16_t a, std::uint16_t b);
    static std::uint16_t gfInv14(std::uint16_t v);
    static std::uint16_t q14(const std::array<std::uint16_t, 6>& words);
    static bool isControlBlock(const PhysicalBlock& block);

    static double estimateFrequency(
        const std::deque<std::pair<std::int64_t, std::int16_t>>& samples);

    void pruneHistory(
        std::int64_t newestPhysicalLine);

    Geometry geometry_;
    int lockFrames_ = 0;
    int badFrames_ = 0;
    int searchCooldownFrames_ = 0;

    std::uint64_t firstCaptureGeneration_ = 0;
    std::int64_t lastProcessedGroup_ = -1;
    std::map<std::int64_t, PhysicalBlock> physicalBlocks_;

    std::uint64_t reconstructedGroups_ = 0;
    std::uint64_t pCorrectedGroups_ = 0;
    std::uint64_t lsbPackMissingGroups_ = 0;
    std::uint64_t hardUncorrectableGroups_ = 0;
    std::uint64_t cleanPChecks_ = 0;
    std::uint64_t cleanPPasses_ = 0;
    std::uint64_t cleanQChecks_ = 0;
    std::uint64_t cleanQPasses_ = 0;

    bool controlValid_ = false;
    bool controlMode16_ = false;
    bool controlPreEmphasis_ = false;

    // Headerless 14/16-bit identification uses a short rolling score rather
    // than lifetime P/Q statistics, so a live mode switch can recover.
    int headerlessModeScore_ = 0; // positive = 14-bit, negative = 16-bit
    bool headerlessModeKnown_ = false;
    bool headerlessMode16_ = false;

    std::deque<std::pair<std::int64_t, std::int16_t>> recentLeft_;
    std::deque<std::pair<std::int64_t, std::int16_t>> recentRight_;
    std::vector<std::int16_t> currentAudioStereo_;
    bool muteBottomTwoBits_ = false;
};
