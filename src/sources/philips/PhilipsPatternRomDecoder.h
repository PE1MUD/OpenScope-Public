#pragma once

#include "video/Yuv444Frame.h"

#include <QString>

#include <array>
#include <cstdint>
#include <vector>

class PhilipsPatternRomDecoder
{
public:
    bool loadIni(
        const QString& iniFileName,
        QString* errorMessage = nullptr);

    bool decodeFrame(
        Yuv444Frame& destination,
        int patternFrame,
        QString* errorMessage = nullptr) const;

    bool alternateFrames() const;

    QString setName() const;
    QString shortName() const;
    QString iniFileName() const;
    double lumaSampleRateHz() const;
    int nativeWidth() const;
    int nativeHeight() const;

private:
    enum class DecoderMode
    {
        Sprite3Byte,
        Field2Byte20MHz
    };

    struct VectorEntry
    {
        std::uint8_t control = 0;
        std::uint8_t sideAddress = 0;
        std::uint8_t centreAddress = 0;
    };

    struct FieldEntry
    {
        std::uint8_t control = 0;
        std::uint8_t addressHigh = 0;
    };

    enum class Segment
    {
        BackPorch,
        Centre,
        FrontPorch
    };

    bool loadRom(
        const QString& role,
        const QString& fileName,
        std::vector<std::uint8_t>& destination,
        QString* errorMessage);

    bool validateConfiguration(
        QString* errorMessage) const;

    bool buildSamplePlanes(
        QString* errorMessage);

    bool applySourceFixes(
        QString* errorMessage);

    bool load3ByteVectorTable(
        QString* errorMessage);

    bool load2ByteFieldTables(
        QString* errorMessage);

    std::size_t decodeVectorAddress(
        const VectorEntry& vector,
        Segment segment,
        int samplesPerAddress) const;

    bool decodeRaw3ByteLine(
        const VectorEntry& vector,
        std::vector<std::uint16_t>& y10,
        std::vector<std::uint8_t>& cb8,
        std::vector<std::uint8_t>& cr8) const;

    bool decodeRaw20MHzLine(
        const FieldEntry& entry,
        std::vector<std::uint16_t>& y10,
        std::vector<std::uint8_t>& cb8,
        std::vector<std::uint8_t>& cr8) const;

    std::vector<int> sourceLineOrder3Byte(
        int patternFrame) const;

    std::vector<int> sourceLineOrder2Byte() const;

    std::uint32_t decode20MHzCentreAddress(
        const FieldEntry& entry) const;

    std::uint16_t mapLumaToDigital10(
        std::uint16_t raw) const;

    std::uint16_t mapRyToDigital10(
        std::uint8_t raw) const;

    std::uint16_t mapByToDigital10(
        std::uint8_t raw) const;

    QString iniFileName_;
    QString setName_;
    QString shortName_;
    QString standard_;
    QString decoderName_;

    DecoderMode decoderMode_ = DecoderMode::Sprite3Byte;

    int vectorTableStart_ = 0;
    int vectorTableLength_ = 0;
    int patternIndex_ = 0;
    int patternFrame_ = 0;
    bool alternateFrames_ = false;

    int backLength_ = 64;
    int centreLength_ = 120;
    int frontLength_ = 32;

    int outputWidth_ = 720;
    int outputHeight_ = 576;
    int cropX_ = 132;

    // Native 20 MHz / two-byte-field profile.
    int nativeWidth_ = 1024;
    int nativeHeight_ = 576;
    int nativeVerticalCropTop_ = 0;
    int fieldLines_ = 290;
    int fieldTableLength_ = 0x244;
    std::array<int, 4> fieldTableStart_{ 0, 0, 0, 0 };
    int lumaBackOffsetSamples_ = 800;
    int chromaBackOffsetSamples_ = 400;

    double lumaSampleRateHz_ = 13'500'000.0;
    double chromaSampleRateHz_ = 6'750'000.0;

    // Chroma timing/gain are set-specific. Phase is expressed in native
    // luminance-sample periods. Positive values place chroma later/right.
    double chromaPhaseLumaSamples_ = 0.0;
    double chromaGain_ = 1.0;

    bool removeChromaBlip_ = false;
    bool reinstateReflectionCheck_ = false;

    int yRawBlack_ = 724;
    int yRawWhite_ = 164;
    int yDigitalBlack_ = 64;
    int yDigitalWhite_ = 940;

    int chromaDigitalCentre_ = 512;
    int chromaDigitalPositive_ = 848;
    int ryRawCentre_ = 128;
    int ryRawAtPositive_ = 63;
    int byRawCentre_ = 128;
    int byRawAtPositive_ = 82;

    std::array<std::vector<std::uint8_t>, 4> lumaMsb_;
    std::vector<std::uint8_t> lumaLsb_;
    std::array<std::vector<std::uint8_t>, 2> chromaRy_;
    std::array<std::vector<std::uint8_t>, 2> chromaBy_;
    std::vector<std::uint8_t> cpuRom_;

    std::vector<std::uint16_t> luma10Samples_;
    std::vector<std::uint8_t> chromaRySamples_;
    std::vector<std::uint8_t> chromaBySamples_;

    std::vector<VectorEntry> vectors_;
    std::array<std::vector<FieldEntry>, 4> fieldEntries_;
};
