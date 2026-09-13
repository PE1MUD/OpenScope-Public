#include "sources/philips/PhilipsPatternRomDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace
{
    constexpr int kLumaSamplesPerAddress = 4;
    constexpr int kChromaSamplesPerAddress = 2;

    std::uint16_t expand10To16(std::uint16_t value)
    {
        return static_cast<std::uint16_t>((value & 0x03ffu) << 6);
    }
}

bool PhilipsPatternRomDecoder::loadIni(
    const QString& iniFileName,
    QString* errorMessage)
{
    iniFileName_ = QFileInfo(iniFileName).absoluteFilePath();

    QSettings settings(iniFileName_, QSettings::IniFormat);

    const auto integerSetting =
        [&settings](const QString& key, int defaultValue)
        {
            const QVariant value = settings.value(key, defaultValue);
            bool ok = false;
            const int parsed = value.toString().toInt(&ok, 0);
            return ok ? parsed : value.toInt();
        };

    const auto doubleSetting =
        [&settings](const QString& key, double defaultValue)
        {
            bool ok = false;
            const double parsed = settings.value(key, defaultValue).toString().toDouble(&ok);
            return ok ? parsed : defaultValue;
        };

    setName_ = settings.value(
        QStringLiteral("Set/Name"),
        QStringLiteral("Philips Pattern ROM")).toString();

    shortName_ = settings.value(
        QStringLiteral("Set/Shortname"),
        setName_).toString().trimmed();

    if (shortName_.isEmpty())
    {
        shortName_ = setName_;
    }

    standard_ = settings.value(
        QStringLiteral("Set/Standard"),
        QStringLiteral("PAL")).toString().trimmed().toUpper();

    decoderName_ = settings.value(
        QStringLiteral("Set/Decoder"),
        QStringLiteral("PM5644_3BYTE")).toString().trimmed().toUpper();

    if (decoderName_ == QStringLiteral("PM5644_2BYTE_20MHZ") ||
        decoderName_ == QStringLiteral("PM5644_G913"))
    {
        decoderMode_ = DecoderMode::Field2Byte20MHz;
    }
    else if (decoderName_ == QStringLiteral("PM5644_3BYTE"))
    {
        decoderMode_ = DecoderMode::Sprite3Byte;
    }
    else
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("Unknown Philips ROM decoder: %1").arg(decoderName_);
        }
        return false;
    }

    vectorTableStart_ = integerSetting(QStringLiteral("VectorTable/Start"), 0x52F6);
    vectorTableLength_ = integerSetting(QStringLiteral("VectorTable/Length"), 0x0D98);
    patternIndex_ = integerSetting(QStringLiteral("Pattern/Index"), 0);
    patternFrame_ = integerSetting(QStringLiteral("Pattern/Frame"), 0);
    alternateFrames_ = settings.value(QStringLiteral("Pattern/AlternateFrames"), false).toBool();

    backLength_ = integerSetting(QStringLiteral("Line/BackAddresses"), 64);
    centreLength_ = integerSetting(QStringLiteral("Line/CentreAddresses"), 120);
    frontLength_ = integerSetting(QStringLiteral("Line/FrontAddresses"), 32);

    outputWidth_ = integerSetting(QStringLiteral("Output/Width"), 720);
    outputHeight_ = integerSetting(QStringLiteral("Output/Height"), 576);
    cropX_ = integerSetting(QStringLiteral("Output/CropX"), 132);

    nativeWidth_ = integerSetting(QStringLiteral("Native/Width"), 1024);
    nativeHeight_ = integerSetting(QStringLiteral("Native/Height"), 576);
    nativeVerticalCropTop_ = integerSetting(QStringLiteral("Native/VerticalCropTop"), 0);
    fieldLines_ = integerSetting(QStringLiteral("Native/FieldLines"), 290);
    lumaBackOffsetSamples_ = integerSetting(QStringLiteral("Native/LumaBackOffsetSamples"), 800);
    chromaBackOffsetSamples_ = integerSetting(QStringLiteral("Native/ChromaBackOffsetSamples"), 400);

    fieldTableStart_[0] = integerSetting(QStringLiteral("FieldTable/Field0Start"), 0x50C8);
    fieldTableStart_[1] = integerSetting(QStringLiteral("FieldTable/Field1Start"), 0x530C);
    fieldTableStart_[2] = integerSetting(QStringLiteral("FieldTable/Field2Start"), 0x5550);
    fieldTableStart_[3] = integerSetting(QStringLiteral("FieldTable/Field3Start"), 0x5794);
    fieldTableLength_ = integerSetting(QStringLiteral("FieldTable/Length"), 0x244);

    lumaSampleRateHz_ = doubleSetting(QStringLiteral("Timing/LumaSampleRateHz"), 13'500'000.0);
    chromaSampleRateHz_ = doubleSetting(QStringLiteral("Timing/ChromaSampleRateHz"), lumaSampleRateHz_ * 0.5);

    chromaPhaseLumaSamples_ =
        doubleSetting(QStringLiteral("Chroma/PhaseLumaSamples"), 0.0);
    chromaGain_ =
        doubleSetting(QStringLiteral("Chroma/Gain"), 1.0);

    removeChromaBlip_ = settings.value(
        QStringLiteral("SourceFixes/RemoveChromaBlip"),
        false).toBool();

    reinstateReflectionCheck_ = settings.value(
        QStringLiteral("SourceFixes/ReinstateReflectionCheck"),
        false).toBool();

    yRawBlack_ = integerSetting(QStringLiteral("Levels/YRawBlack"), 724);
    yRawWhite_ = integerSetting(QStringLiteral("Levels/YRawWhite"), 164);
    yDigitalBlack_ = integerSetting(QStringLiteral("Levels/YDigitalBlack"), 64);
    yDigitalWhite_ = integerSetting(QStringLiteral("Levels/YDigitalWhite"), 940);

    chromaDigitalCentre_ = integerSetting(QStringLiteral("Levels/ChromaDigitalCentre"), 512);
    chromaDigitalPositive_ = integerSetting(QStringLiteral("Levels/ChromaDigitalPositive"), 848);
    ryRawCentre_ = integerSetting(QStringLiteral("Levels/RYRawCentre"), 128);
    ryRawAtPositive_ = integerSetting(QStringLiteral("Levels/RYRawAtPositive"), 63);
    byRawCentre_ = integerSetting(QStringLiteral("Levels/BYRawCentre"), 128);
    byRawAtPositive_ = integerSetting(QStringLiteral("Levels/BYRawAtPositive"), 82);

    const QDir romDirectory(QFileInfo(iniFileName_).absolutePath());

    const auto romFileName =
        [&settings, &romDirectory](const QString& role)
        {
            const QString configured = settings.value(QStringLiteral("ROM/") + role).toString().trimmed();
            if (configured.isEmpty())
            {
                return QString();
            }
            return QFileInfo(configured).isAbsolute() ? configured : romDirectory.filePath(configured);
        };

    for (int index = 0; index < 4; ++index)
    {
        const QString role = QStringLiteral("Luma%1").arg(index);
        if (!loadRom(role, romFileName(role), lumaMsb_[static_cast<std::size_t>(index)], errorMessage))
        {
            return false;
        }
    }

    if (!loadRom(QStringLiteral("LumaLSB"), romFileName(QStringLiteral("LumaLSB")), lumaLsb_, errorMessage) ||
        !loadRom(QStringLiteral("RY0"), romFileName(QStringLiteral("RY0")), chromaRy_[0], errorMessage) ||
        !loadRom(QStringLiteral("RY1"), romFileName(QStringLiteral("RY1")), chromaRy_[1], errorMessage) ||
        !loadRom(QStringLiteral("BY0"), romFileName(QStringLiteral("BY0")), chromaBy_[0], errorMessage) ||
        !loadRom(QStringLiteral("BY1"), romFileName(QStringLiteral("BY1")), chromaBy_[1], errorMessage) ||
        !loadRom(QStringLiteral("CPU"), romFileName(QStringLiteral("CPU")), cpuRom_, errorMessage))
    {
        return false;
    }

    if (!validateConfiguration(errorMessage) ||
        !buildSamplePlanes(errorMessage) ||
        !applySourceFixes(errorMessage))
    {
        return false;
    }

    vectors_.clear();
    for (auto& field : fieldEntries_)
    {
        field.clear();
    }

    return decoderMode_ == DecoderMode::Sprite3Byte
        ? load3ByteVectorTable(errorMessage)
        : load2ByteFieldTables(errorMessage);
}

bool PhilipsPatternRomDecoder::decodeFrame(
    Yuv444Frame& destination,
    int patternFrame,
    QString* errorMessage) const
{
    if (decoderMode_ == DecoderMode::Field2Byte20MHz)
    {
        const int selectedFrame = alternateFrames_ ? std::clamp(patternFrame, 0, 1) : std::clamp(patternFrame_, 0, 1);
        const int firstField = selectedFrame * 2;
        const int secondField = firstField + 1;

        if (fieldEntries_[static_cast<std::size_t>(firstField)].size() != static_cast<std::size_t>(fieldLines_) ||
            fieldEntries_[static_cast<std::size_t>(secondField)].size() != static_cast<std::size_t>(fieldLines_))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = QStringLiteral("20 MHz field table is not loaded correctly.");
            }
            return false;
        }

        const std::vector<int> order = sourceLineOrder2Byte();
        if (nativeVerticalCropTop_ < 0 ||
            nativeHeight_ <= 0 ||
            nativeVerticalCropTop_ + nativeHeight_ > static_cast<int>(order.size()))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = QStringLiteral("20 MHz field reconstruction produced %1 lines; crop %2 + height %3 is invalid.")
                    .arg(order.size()).arg(nativeVerticalCropTop_).arg(nativeHeight_);
            }
            return false;
        }

        destination.resize(nativeWidth_, nativeHeight_);
        destination.sampleClockHz = lumaSampleRateHz_;

        std::vector<std::uint16_t> rawY;
        std::vector<std::uint8_t> rawCb;
        std::vector<std::uint8_t> rawCr;

        for (int outputLine = 0; outputLine < nativeHeight_; ++outputLine)
        {
            const int combinedIndex = order[static_cast<std::size_t>(nativeVerticalCropTop_ + outputLine)];
            const bool useSecond = combinedIndex >= fieldLines_;
            const int lineIndex = useSecond ? combinedIndex - fieldLines_ : combinedIndex;
            const int fieldIndex = useSecond ? secondField : firstField;

            const FieldEntry& entry = fieldEntries_[static_cast<std::size_t>(fieldIndex)][static_cast<std::size_t>(lineIndex)];

            if (!decodeRaw20MHzLine(entry, rawY, rawCb, rawCr))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = QStringLiteral("20 MHz line %1 references samples outside the ROM data.").arg(lineIndex);
                }
                return false;
            }

            const std::size_t destinationOffset = static_cast<std::size_t>(outputLine) * static_cast<std::size_t>(nativeWidth_);

            for (int x = 0; x < nativeWidth_; ++x)
            {
                const std::size_t outputIndex = destinationOffset + static_cast<std::size_t>(x);
                destination.y[outputIndex] = expand10To16(mapLumaToDigital10(rawY[static_cast<std::size_t>(x)]));

                // Chroma is stored at half the luma sample rate.  Keep the
                // temporal relationship explicit instead of assuming that
                // chroma sample 0 is exactly co-sited with luma sample 0.
                //
                // PhaseLumaSamples is the chroma-sample-0 position in luma
                // sample periods. Positive phase therefore moves chroma to
                // the right/later relative to luma.
                const double chromaPosition =
                    (static_cast<double>(x) - chromaPhaseLumaSamples_) *
                    (chromaSampleRateHz_ / lumaSampleRateHz_);

                const double clampedChromaPosition =
                    std::clamp(
                        chromaPosition,
                        0.0,
                        static_cast<double>(rawCb.size() - 1u));

                const std::size_t cIndex =
                    static_cast<std::size_t>(std::floor(clampedChromaPosition));
                const std::size_t cNext =
                    std::min(cIndex + 1u, rawCb.size() - 1u);
                const double fraction =
                    clampedChromaPosition - static_cast<double>(cIndex);

                const auto interpolateChroma =
                    [fraction](std::uint16_t left, std::uint16_t right)
                    {
                        return static_cast<std::uint16_t>(
                            std::clamp(
                                static_cast<int>(std::lround(
                                    static_cast<double>(left) +
                                    (static_cast<double>(right) -
                                     static_cast<double>(left)) * fraction)),
                                0,
                                1023));
                    };

                const std::uint16_t cbUngained = interpolateChroma(
                    mapByToDigital10(rawCb[cIndex]),
                    mapByToDigital10(rawCb[cNext]));
                const std::uint16_t crUngained = interpolateChroma(
                    mapRyToDigital10(rawCr[cIndex]),
                    mapRyToDigital10(rawCr[cNext]));

                const auto applyChromaGain =
                    [this](std::uint16_t value)
                    {
                        const double excursion =
                            static_cast<double>(value) -
                            static_cast<double>(chromaDigitalCentre_);
                        return static_cast<std::uint16_t>(
                            std::clamp(
                                static_cast<int>(std::lround(
                                    static_cast<double>(chromaDigitalCentre_) +
                                    excursion * chromaGain_)),
                                0,
                                1023));
                    };

                destination.u[outputIndex] =
                    expand10To16(applyChromaGain(cbUngained));
                destination.v[outputIndex] =
                    expand10To16(applyChromaGain(crUngained));
            }
        }

        return true;
    }

    if (vectors_.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("No Philips ROM set is loaded.");
        }
        return false;
    }

    const std::vector<int> lineOrder = sourceLineOrder3Byte(patternFrame);
    if (static_cast<int>(lineOrder.size()) != outputHeight_)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("Configured output height is %1, but source reconstruction produced %2 lines.")
                .arg(outputHeight_).arg(lineOrder.size());
        }
        return false;
    }

    const int rawLumaWidth = (backLength_ + centreLength_ + frontLength_) * kLumaSamplesPerAddress;
    const int rawChromaWidth = (backLength_ + centreLength_ + frontLength_) * kChromaSamplesPerAddress;

    if (cropX_ < 0 || outputWidth_ <= 0 || cropX_ + outputWidth_ > rawLumaWidth)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("Output crop %1 + %2 exceeds raw line width %3.")
                .arg(cropX_).arg(outputWidth_).arg(rawLumaWidth);
        }
        return false;
    }

    destination.resize(outputWidth_, outputHeight_);
    destination.sampleClockHz = lumaSampleRateHz_;

    std::vector<std::uint16_t> rawY;
    std::vector<std::uint8_t> rawCb;
    std::vector<std::uint8_t> rawCr;
    rawY.reserve(static_cast<std::size_t>(rawLumaWidth));
    rawCb.reserve(static_cast<std::size_t>(rawChromaWidth));
    rawCr.reserve(static_cast<std::size_t>(rawChromaWidth));

    for (int outputLine = 0; outputLine < outputHeight_; ++outputLine)
    {
        const int vectorIndex = lineOrder[static_cast<std::size_t>(outputLine)];
        if (vectorIndex < 0 || vectorIndex >= static_cast<int>(vectors_.size()) ||
            !decodeRaw3ByteLine(vectors_[static_cast<std::size_t>(vectorIndex)], rawY, rawCb, rawCr))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = QStringLiteral("Vector %1 references samples outside the ROM data.").arg(vectorIndex);
            }
            return false;
        }

        const std::size_t destinationOffset = static_cast<std::size_t>(outputLine) * static_cast<std::size_t>(outputWidth_);

        for (int x = 0; x < outputWidth_; ++x)
        {
            const int rawX = cropX_ + x;
            const std::size_t outputIndex = destinationOffset + static_cast<std::size_t>(x);
            destination.y[outputIndex] = expand10To16(mapLumaToDigital10(rawY[static_cast<std::size_t>(rawX)]));

            // The 3-byte/G00 family has the same 2:1 luma/chroma sample
            // relationship as the 20 MHz family.  Apply the configured
            // chroma phase here as well; previously PhaseLumaSamples was
            // parsed from rom.ini but silently ignored on this path.
            const double chromaPosition =
                (static_cast<double>(rawX) - chromaPhaseLumaSamples_) *
                (chromaSampleRateHz_ / lumaSampleRateHz_);

            const double clampedChromaPosition =
                std::clamp(
                    chromaPosition,
                    0.0,
                    static_cast<double>(rawCb.size() - 1u));

            const std::size_t cIndex =
                static_cast<std::size_t>(std::floor(clampedChromaPosition));
            const std::size_t cNext =
                std::min(cIndex + 1u, rawCb.size() - 1u);
            const double fraction =
                clampedChromaPosition - static_cast<double>(cIndex);

            const auto interpolateChroma =
                [fraction](std::uint16_t left, std::uint16_t right)
                {
                    return static_cast<std::uint16_t>(
                        std::clamp(
                            static_cast<int>(std::lround(
                                static_cast<double>(left) +
                                (static_cast<double>(right) -
                                 static_cast<double>(left)) * fraction)),
                            0,
                            1023));
                };

            const auto applyChromaGain =
                [this](std::uint16_t value)
                {
                    const double excursion =
                        static_cast<double>(value) -
                        static_cast<double>(chromaDigitalCentre_);

                    return static_cast<std::uint16_t>(
                        std::clamp(
                            static_cast<int>(std::lround(
                                static_cast<double>(chromaDigitalCentre_) +
                                excursion * chromaGain_)),
                            0,
                            1023));
                };

            const std::uint16_t cb = applyChromaGain(
                interpolateChroma(
                    mapByToDigital10(rawCb[cIndex]),
                    mapByToDigital10(rawCb[cNext])));

            const std::uint16_t cr = applyChromaGain(
                interpolateChroma(
                    mapRyToDigital10(rawCr[cIndex]),
                    mapRyToDigital10(rawCr[cNext])));

            destination.u[outputIndex] = expand10To16(cb);
            destination.v[outputIndex] = expand10To16(cr);
        }
    }

    return true;
}

bool PhilipsPatternRomDecoder::alternateFrames() const
{
    return alternateFrames_;
}

QString PhilipsPatternRomDecoder::setName() const
{
    return setName_;
}

QString PhilipsPatternRomDecoder::shortName() const
{
    return shortName_;
}

QString PhilipsPatternRomDecoder::iniFileName() const
{
    return iniFileName_;
}

double PhilipsPatternRomDecoder::lumaSampleRateHz() const
{
    return lumaSampleRateHz_;
}

int PhilipsPatternRomDecoder::nativeWidth() const
{
    return decoderMode_ == DecoderMode::Field2Byte20MHz ? nativeWidth_ : outputWidth_;
}

int PhilipsPatternRomDecoder::nativeHeight() const
{
    return decoderMode_ == DecoderMode::Field2Byte20MHz ? nativeHeight_ : outputHeight_;
}

bool PhilipsPatternRomDecoder::loadRom(
    const QString& role,
    const QString& fileName,
    std::vector<std::uint8_t>& destination,
    QString* errorMessage)
{
    if (fileName.trimmed().isEmpty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("ROM role %1 is missing from rom.ini.").arg(role);
        }
        return false;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("Could not open %1 ROM: %2").arg(role, fileName);
        }
        return false;
    }

    const QByteArray bytes = file.readAll();
    destination.assign(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

bool PhilipsPatternRomDecoder::validateConfiguration(QString* errorMessage) const
{
    if (standard_ != QStringLiteral("PAL"))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("This OpenScope Philips decoder currently supports PAL sets only.");
        }
        return false;
    }

    if (lumaSampleRateHz_ <= 0.0 || chromaSampleRateHz_ <= 0.0 ||
        !std::isfinite(chromaPhaseLumaSamples_) ||
        !std::isfinite(chromaGain_) || chromaGain_ <= 0.0 ||
        yRawBlack_ == yRawWhite_ || ryRawCentre_ == ryRawAtPositive_ || byRawCentre_ == byRawAtPositive_)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("rom.ini contains an invalid timing or level setting.");
        }
        return false;
    }

    if (decoderMode_ == DecoderMode::Field2Byte20MHz &&
        (nativeWidth_ <= 0 || nativeHeight_ <= 0 || nativeVerticalCropTop_ < 0 || fieldLines_ <= 0 || fieldTableLength_ != fieldLines_ * 2 ||
         lumaBackOffsetSamples_ < 0 || chromaBackOffsetSamples_ < 0))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("rom.ini contains an invalid 20 MHz field-table configuration.");
        }
        return false;
    }

    const std::size_t romSize = lumaMsb_[0].size();
    if (romSize == 0u)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral("Luma0 ROM is empty.");
        }
        return false;
    }

    const auto sameSize = [romSize](const std::vector<std::uint8_t>& data) { return data.size() == romSize; };
    for (const auto& rom : lumaMsb_)
    {
        if (!sameSize(rom))
        {
            if (errorMessage != nullptr) *errorMessage = QStringLiteral("The four luma MSB ROMs do not have equal size.");
            return false;
        }
    }

    if (!sameSize(lumaLsb_) || !sameSize(chromaRy_[0]) || !sameSize(chromaRy_[1]) ||
        !sameSize(chromaBy_[0]) || !sameSize(chromaBy_[1]) || cpuRom_.empty())
    {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Video ROM sizes do not match, or CPU ROM is empty.");
        return false;
    }

    return true;
}

bool PhilipsPatternRomDecoder::buildSamplePlanes(QString*)
{
    const std::size_t romSize = lumaMsb_[0].size();
    luma10Samples_.resize(romSize * 4u);
    chromaRySamples_.resize(romSize * 2u);
    chromaBySamples_.resize(romSize * 2u);

    for (std::size_t address = 0; address < romSize; ++address)
    {
        const std::uint8_t lsb = lumaLsb_[address];
        luma10Samples_[address * 4u + 0u] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(lumaMsb_[0][address]) << 2) | ((lsb & 0x10u) >> 3) | (lsb & 0x01u));
        luma10Samples_[address * 4u + 1u] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(lumaMsb_[1][address]) << 2) | ((lsb & 0x20u) >> 4) | ((lsb & 0x02u) >> 1));
        luma10Samples_[address * 4u + 2u] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(lumaMsb_[2][address]) << 2) | ((lsb & 0x40u) >> 5) | ((lsb & 0x04u) >> 2));
        luma10Samples_[address * 4u + 3u] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(lumaMsb_[3][address]) << 2) | ((lsb & 0x80u) >> 6) | ((lsb & 0x08u) >> 3));
        chromaRySamples_[address * 2u + 0u] = chromaRy_[0][address];
        chromaRySamples_[address * 2u + 1u] = chromaRy_[1][address];
        chromaBySamples_[address * 2u + 0u] = chromaBy_[0][address];
        chromaBySamples_[address * 2u + 1u] = chromaBy_[1][address];
    }
    return true;
}

bool PhilipsPatternRomDecoder::applySourceFixes(QString* errorMessage)
{
    if (decoderMode_ != DecoderMode::Sprite3Byte)
    {
        return true;
    }

    if (removeChromaBlip_)
    {
        // Exact PM5644 G00 cleanup from the supplied PhilipsPatternRom
        // converter. It removes the small R-Y "hook" at the right-hand
        // side of the circle.
        constexpr std::array<std::size_t, 6> kRyIndices{
            (0x5E70u * 2u) - 1u,
            (0x5EF0u * 2u),
            (0x5F70u * 2u),
            (0x5FF0u * 2u),
            (0x6070u * 2u),
            (0x60F0u * 2u)
        };

        for (const std::size_t index : kRyIndices)
        {
            if (index >= chromaRySamples_.size())
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = QStringLiteral(
                        "G00 chroma-blip source fix is outside the R-Y sample ROM.");
                }
                return false;
            }

            chromaRySamples_[index] = 0x8Au;
        }
    }

    if (reinstateReflectionCheck_)
    {
        // Exact luminance pulse and source addresses from the supplied
        // PhilipsPatternRom converter (RomManager::ApplySourceFixes).
        constexpr std::array<std::uint16_t, 8> kPulse{
            724u, 636u, 367u, 174u, 244u, 510u, 712u, 724u
        };

        constexpr std::array<std::size_t, 40> kLineStarts{
            141454u, 163982u, 164494u, 165006u, 165518u,
            166030u, 166542u, 167054u, 167566u, 168078u,
            168590u, 169102u, 169614u, 170126u, 170638u,
            171150u, 171662u, 172174u, 172686u, 173710u,
            173198u, 131726u, 174222u, 174734u, 175246u,
            175758u, 176270u, 176782u, 177294u, 177806u,
            178318u, 178830u, 179342u, 179854u, 180366u,
            180878u, 181390u, 181902u, 182414u, 182926u
        };

        for (const std::size_t start : kLineStarts)
        {
            if (start > luma10Samples_.size() ||
                kPulse.size() > luma10Samples_.size() - start)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = QStringLiteral(
                        "G00 reflection-check source fix is outside the luma sample ROM.");
                }
                return false;
            }

            std::copy(
                kPulse.begin(),
                kPulse.end(),
                luma10Samples_.begin() + static_cast<std::ptrdiff_t>(start));
        }
    }

    return true;
}

bool PhilipsPatternRomDecoder::load3ByteVectorTable(QString* errorMessage)
{
    const std::size_t tableStart = static_cast<std::size_t>(vectorTableStart_) +
        static_cast<std::size_t>(vectorTableLength_) * static_cast<std::size_t>(patternIndex_);
    const std::size_t tableLength = static_cast<std::size_t>(vectorTableLength_);

    if (tableStart > cpuRom_.size() || tableLength > cpuRom_.size() - tableStart || (tableLength % 3u) != 0u)
    {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("3-byte vector table is outside CPU ROM or has invalid length.");
        return false;
    }

    vectors_.reserve(tableLength / 3u);
    for (std::size_t offset = 0; offset < tableLength; offset += 3u)
    {
        vectors_.push_back(VectorEntry{
            cpuRom_[tableStart + offset + 0u],
            cpuRom_[tableStart + offset + 1u],
            cpuRom_[tableStart + offset + 2u] });
    }
    return true;
}

bool PhilipsPatternRomDecoder::load2ByteFieldTables(QString* errorMessage)
{
    for (std::size_t fieldIndex = 0; fieldIndex < fieldEntries_.size(); ++fieldIndex)
    {
        const int configuredStart = fieldTableStart_[fieldIndex];
        if (configuredStart < 0)
        {
            if (errorMessage != nullptr) *errorMessage = QStringLiteral("Negative field-table start.");
            return false;
        }

        const std::size_t start = static_cast<std::size_t>(configuredStart);
        const std::size_t length = static_cast<std::size_t>(fieldTableLength_);
        if (start > cpuRom_.size() || length > cpuRom_.size() - start || (length % 2u) != 0u)
        {
            if (errorMessage != nullptr) *errorMessage = QStringLiteral("20 MHz field table %1 is outside CPU ROM.").arg(fieldIndex);
            return false;
        }

        auto& destination = fieldEntries_[fieldIndex];
        destination.reserve(length / 2u);
        for (std::size_t offset = 0; offset < length; offset += 2u)
        {
            destination.push_back(FieldEntry{
                cpuRom_[start + offset + 0u],
                cpuRom_[start + offset + 1u] });
        }
    }
    return true;
}

std::size_t PhilipsPatternRomDecoder::decodeVectorAddress(
    const VectorEntry& vector,
    Segment segment,
    int samplesPerAddress) const
{
    std::array<std::uint8_t, 3> lsbSequence{};
    switch (vector.control & 0x03u)
    {
    case 0: lsbSequence = { 0x00u, 0x00u, 0x40u }; break;
    case 1: lsbSequence = { 0x00u, 0x80u, 0x40u }; break;
    case 2: lsbSequence = { 0x80u, 0x00u, 0xC0u }; break;
    case 3: lsbSequence = { 0x80u, 0x80u, 0xC0u }; break;
    }

    std::uint32_t address = 0;
    switch (segment)
    {
    case Segment::BackPorch: address = (static_cast<std::uint32_t>(vector.sideAddress) << 8) | lsbSequence[0]; break;
    case Segment::Centre: address = (static_cast<std::uint32_t>(vector.centreAddress) << 8) | lsbSequence[1]; break;
    case Segment::FrontPorch: address = (static_cast<std::uint32_t>(vector.sideAddress) << 8) | lsbSequence[2]; break;
    }

    if ((address & 0x100u) == 0u && (vector.control & 0x20u) != 0u) address |= 0x10000u;
    if ((address & 0x100u) != 0u && (vector.control & 0x10u) != 0u) address |= 0x10000u;
    if ((vector.control & 0x04u) != 0u) address |= 0x20000u;
    if ((vector.control & 0x08u) != 0u) address |= 0x40000u;

    return static_cast<std::size_t>(address) * static_cast<std::size_t>(samplesPerAddress);
}

bool PhilipsPatternRomDecoder::decodeRaw3ByteLine(
    const VectorEntry& vector,
    std::vector<std::uint16_t>& y10,
    std::vector<std::uint8_t>& cb8,
    std::vector<std::uint8_t>& cr8) const
{
    y10.clear(); cb8.clear(); cr8.clear();

    const auto append = [](const auto& source, std::size_t start, std::size_t count, auto& destination)
    {
        if (start > source.size() || count > source.size() - start) return false;
        destination.insert(destination.end(), source.begin() + static_cast<std::ptrdiff_t>(start), source.begin() + static_cast<std::ptrdiff_t>(start + count));
        return true;
    };

    const std::array<Segment, 3> segments{ Segment::BackPorch, Segment::Centre, Segment::FrontPorch };
    const std::array<int, 3> lengths{ backLength_, centreLength_, frontLength_ };

    for (std::size_t index = 0; index < segments.size(); ++index)
    {
        const std::size_t yStart = decodeVectorAddress(vector, segments[index], kLumaSamplesPerAddress);
        const std::size_t cStart = decodeVectorAddress(vector, segments[index], kChromaSamplesPerAddress);
        if (!append(luma10Samples_, yStart, static_cast<std::size_t>(lengths[index]) * 4u, y10) ||
            !append(chromaBySamples_, cStart, static_cast<std::size_t>(lengths[index]) * 2u, cb8) ||
            !append(chromaRySamples_, cStart, static_cast<std::size_t>(lengths[index]) * 2u, cr8)) return false;
    }
    return true;
}

std::uint32_t PhilipsPatternRomDecoder::decode20MHzCentreAddress(const FieldEntry& entry) const
{
    std::uint32_t address = static_cast<std::uint32_t>(entry.addressHigh) << 8;
    if ((entry.control & 0x20u) != 0u) address |= 0x10000u;
    if ((entry.control & 0x04u) != 0u) address |= 0x20000u;
    if ((entry.control & 0x08u) != 0u) address |= 0x40000u;
    return address;
}

bool PhilipsPatternRomDecoder::decodeRaw20MHzLine(
    const FieldEntry& entry,
    std::vector<std::uint16_t>& y10,
    std::vector<std::uint8_t>& cb8,
    std::vector<std::uint8_t>& cr8) const
{
    const std::uint32_t centreAddress = decode20MHzCentreAddress(entry);
    const std::int64_t yStartSigned = static_cast<std::int64_t>(centreAddress) * 4 - lumaBackOffsetSamples_;
    const std::int64_t cStartSigned = static_cast<std::int64_t>(centreAddress) * 2 - chromaBackOffsetSamples_;
    const std::size_t chromaWidth = static_cast<std::size_t>((nativeWidth_ + 1) / 2);

    if (yStartSigned < 0 || cStartSigned < 0) return false;
    const std::size_t yStart = static_cast<std::size_t>(yStartSigned);
    const std::size_t cStart = static_cast<std::size_t>(cStartSigned);
    if (yStart > luma10Samples_.size() || static_cast<std::size_t>(nativeWidth_) > luma10Samples_.size() - yStart ||
        cStart > chromaBySamples_.size() || chromaWidth > chromaBySamples_.size() - cStart ||
        cStart > chromaRySamples_.size() || chromaWidth > chromaRySamples_.size() - cStart) return false;

    y10.assign(luma10Samples_.begin() + static_cast<std::ptrdiff_t>(yStart),
        luma10Samples_.begin() + static_cast<std::ptrdiff_t>(yStart + static_cast<std::size_t>(nativeWidth_)));
    cb8.assign(chromaBySamples_.begin() + static_cast<std::ptrdiff_t>(cStart),
        chromaBySamples_.begin() + static_cast<std::ptrdiff_t>(cStart + chromaWidth));
    cr8.assign(chromaRySamples_.begin() + static_cast<std::ptrdiff_t>(cStart),
        chromaRySamples_.begin() + static_cast<std::ptrdiff_t>(cStart + chromaWidth));
    return true;
}

std::vector<int> PhilipsPatternRomDecoder::sourceLineOrder3Byte(int patternFrame) const
{
    std::vector<int> order;
    if (vectors_.empty() || (vectors_.size() % 4u) != 0u) return order;
    const int linesPerField = static_cast<int>(vectors_.size() / 4u);
    const int selectedPatternFrame = alternateFrames_ ? std::clamp(patternFrame, 0, 1) : patternFrame_;
    const int startLine = linesPerField * 2 * selectedPatternFrame;
    if (linesPerField < 4 || startLine < 0 || startLine + linesPerField * 2 > static_cast<int>(vectors_.size())) return order;

    order.reserve(static_cast<std::size_t>(linesPerField * 2 - 4));
    order.push_back(startLine + 0);
    order.push_back(startLine + linesPerField - 1);
    for (int line = 2; line < linesPerField - 2; ++line)
    {
        order.push_back(startLine + line + linesPerField);
        order.push_back(startLine + line);
    }
    order.push_back(startLine + 1);
    order.push_back(startLine + linesPerField + 1);
    return order;
}

std::vector<int> PhilipsPatternRomDecoder::sourceLineOrder2Byte() const
{
    std::vector<int> order;
    if (fieldLines_ < 4) return order;
    order.reserve(static_cast<std::size_t>(fieldLines_ * 2 - 4));
    order.push_back(0);
    order.push_back(fieldLines_ - 1);
    for (int line = 2; line < fieldLines_ - 2; ++line)
    {
        order.push_back(line + fieldLines_);
        order.push_back(line);
    }
    order.push_back(1);
    order.push_back(fieldLines_ + 1);
    return order;
}

std::uint16_t PhilipsPatternRomDecoder::mapLumaToDigital10(std::uint16_t raw) const
{
    const double scale = static_cast<double>(yDigitalWhite_ - yDigitalBlack_) / static_cast<double>(yRawWhite_ - yRawBlack_);
    const double mapped = static_cast<double>(yDigitalBlack_) + (static_cast<double>(raw) - yRawBlack_) * scale;
    return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(mapped)), 0, 1023));
}

std::uint16_t PhilipsPatternRomDecoder::mapRyToDigital10(std::uint8_t raw) const
{
    const double scale = static_cast<double>(chromaDigitalPositive_ - chromaDigitalCentre_) / static_cast<double>(ryRawAtPositive_ - ryRawCentre_);
    const double mapped = static_cast<double>(chromaDigitalCentre_) + (static_cast<double>(raw) - ryRawCentre_) * scale;
    return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(mapped)), 0, 1023));
}

std::uint16_t PhilipsPatternRomDecoder::mapByToDigital10(std::uint8_t raw) const
{
    const double scale = static_cast<double>(chromaDigitalPositive_ - chromaDigitalCentre_) / static_cast<double>(byRawAtPositive_ - byRawCentre_);
    const double mapped = static_cast<double>(chromaDigitalCentre_) + (static_cast<double>(raw) - byRawCentre_) * scale;
    return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(mapped)), 0, 1023));
}
