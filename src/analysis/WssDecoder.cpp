#include "WssDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    constexpr int kRunInElementCount = 29;
    constexpr std::uint32_t kRunInPattern = 0x1F1C71C7u;

    constexpr int kStartCodeElementCount = 24;
    constexpr std::uint32_t kStartCodePattern = 0x001E3C1Fu;

    constexpr int kPrefixElementCount =
        kRunInElementCount + kStartCodeElementCount;

    constexpr int kDataBitCount = 14;
    constexpr int kElementsPerDataBit = 6;
    constexpr int kTotalElementCount =
        kPrefixElementCount +
        kDataBitCount * kElementsPerDataBit;

    constexpr double kWssClockHz = 5'000'000.0;
    constexpr int kSearchLineCount = 8;

    std::vector<int> makePrefixPattern()
    {
        std::vector<int> result;
        result.reserve(kPrefixElementCount);

        for (int bit = kRunInElementCount - 1;
             bit >= 0;
             --bit)
        {
            result.push_back(
                ((kRunInPattern >> bit) & 1u) != 0u
                ? 1
                : 0);
        }

        for (int bit = kStartCodeElementCount - 1;
             bit >= 0;
             --bit)
        {
            result.push_back(
                ((kStartCodePattern >> bit) & 1u) != 0u
                ? 1
                : 0);
        }

        return result;
    }

    const std::vector<int>& prefixPattern()
    {
        static const std::vector<int> pattern =
            makePrefixPattern();

        return pattern;
    }

    double sampleLinear(
        const std::uint16_t* line,
        int width,
        double position)
    {
        const int left =
            std::clamp(
                static_cast<int>(std::floor(position)),
                0,
                width - 1);

        const int right =
            std::min(
                left + 1,
                width - 1);

        const double fraction =
            position -
            static_cast<double>(left);

        return
            static_cast<double>(line[left]) *
                (1.0 - fraction) +
            static_cast<double>(line[right]) *
                fraction;
    }

    struct PrefixScore
    {
        double score = 0.0;
        double contrast = 0.0;
    };

    PrefixScore scorePrefix(
        const std::uint16_t* line,
        int width,
        double start,
        double samplesPerElement)
    {
        const auto& pattern =
            prefixPattern();

        std::array<double, kPrefixElementCount>
            samples{};

        double highSum = 0.0;
        double lowSum = 0.0;
        int highCount = 0;
        int lowCount = 0;

        for (int element = 0;
             element < kPrefixElementCount;
             ++element)
        {
            const double value =
                sampleLinear(
                    line,
                    width,
                    start +
                    (static_cast<double>(element) + 0.5) *
                        samplesPerElement);

            samples[
                static_cast<std::size_t>(
                    element)] =
                value;

            if (pattern[
                    static_cast<std::size_t>(
                        element)] != 0)
            {
                highSum += value;
                ++highCount;
            }
            else
            {
                lowSum += value;
                ++lowCount;
            }
        }

        if (highCount == 0 ||
            lowCount == 0)
        {
            return {};
        }

        const double highMean =
            highSum /
            static_cast<double>(highCount);

        const double lowMean =
            lowSum /
            static_cast<double>(lowCount);

        const double contrast =
            highMean - lowMean;

        constexpr double minimumContrast = 1200.0;

        if (contrast <
            minimumContrast)
        {
            return {};
        }

        const double midpoint =
            (highMean + lowMean) * 0.5;

        double signedCorrelation = 0.0;
        double absoluteEnergy = 0.0;

        for (int element = 0;
             element < kPrefixElementCount;
             ++element)
        {
            const double centered =
                samples[
                    static_cast<std::size_t>(
                        element)] -
                midpoint;

            signedCorrelation +=
                (pattern[
                    static_cast<std::size_t>(
                        element)] != 0
                    ? 1.0
                    : -1.0) *
                centered;

            absoluteEnergy +=
                std::abs(centered);
        }

        if (absoluteEnergy <= 1.0)
        {
            return {};
        }

        return {
            signedCorrelation / absoluteEnergy,
            contrast
        };
    }

    bool hasOddGroup1Parity(
        const std::array<int, kDataBitCount>& bits)
    {
        return
            ((bits[0] +
              bits[1] +
              bits[2] +
              bits[3]) &
             1) != 0;
    }
}

WssDecoder::Candidate WssDecoder::decodeLine(
    const std::uint16_t* line,
    int width,
    double sampleClockHz,
    int lineIndex)
{
    Candidate best;

    if (line == nullptr ||
        width <= 0 ||
        sampleClockHz <
            1'000'000.0)
    {
        return best;
    }

    const double samplesPerElement =
        sampleClockHz /
        kWssClockHz;

    const double requiredSamples =
        static_cast<double>(
            kTotalElementCount) *
        samplesPerElement;

    if (requiredSamples >=
        static_cast<double>(width))
    {
        return best;
    }

    const int maximumStart =
        std::max(
            0,
            static_cast<int>(
                std::floor(
                    static_cast<double>(width) -
                    requiredSamples -
                    1.0)));

    double bestStart = 0.0;
    double bestPrefixScore = 0.0;
    double bestContrast = 0.0;

    for (int start = 0;
         start <= maximumStart;
         ++start)
    {
        const PrefixScore score =
            scorePrefix(
                line,
                width,
                static_cast<double>(start),
                samplesPerElement);

        if (score.score >
            bestPrefixScore)
        {
            bestPrefixScore = score.score;
            bestContrast = score.contrast;
            bestStart =
                static_cast<double>(start);
        }
    }

    for (int tenth = -10;
         tenth <= 10;
         ++tenth)
    {
        const double candidateStart =
            bestStart +
            static_cast<double>(tenth) *
                0.1;

        if (candidateStart < 0.0 ||
            candidateStart >
                static_cast<double>(
                    maximumStart))
        {
            continue;
        }

        const PrefixScore score =
            scorePrefix(
                line,
                width,
                candidateStart,
                samplesPerElement);

        if (score.score >
            bestPrefixScore)
        {
            bestPrefixScore = score.score;
            bestContrast = score.contrast;
            bestStart = candidateStart;
        }
    }

    constexpr double minimumPrefixScore = 0.78;

    if (bestPrefixScore <
        minimumPrefixScore)
    {
        return best;
    }

    std::array<int, kDataBitCount>
        bits{};

    const double dataStart =
        bestStart +
        static_cast<double>(
            kPrefixElementCount) *
            samplesPerElement;

    for (int bit = 0;
         bit < kDataBitCount;
         ++bit)
    {
        double firstHalf = 0.0;
        double secondHalf = 0.0;

        for (int element = 0;
             element < 3;
             ++element)
        {
            firstHalf +=
                sampleLinear(
                    line,
                    width,
                    dataStart +
                    (static_cast<double>(
                        bit * kElementsPerDataBit +
                        element) +
                     0.5) *
                    samplesPerElement);

            secondHalf +=
                sampleLinear(
                    line,
                    width,
                    dataStart +
                    (static_cast<double>(
                        bit * kElementsPerDataBit +
                        3 +
                        element) +
                     0.5) *
                    samplesPerElement);
        }

        firstHalf /= 3.0;
        secondHalf /= 3.0;

        const double transition =
            firstHalf -
            secondHalf;

        if (std::abs(transition) <
            bestContrast * 0.20)
        {
            return best;
        }

        // ETSI EN 300 294:
        //   0 = 000111
        //   1 = 111000
        bits[
            static_cast<std::size_t>(
                bit)] =
            transition > 0.0
            ? 1
            : 0;
    }

    if (!hasOddGroup1Parity(bits))
    {
        return best;
    }

    best.valid = true;
    best.line = lineIndex;
    best.formatCode =
        bits[0] |
        (bits[1] << 1) |
        (bits[2] << 2);
    best.score = bestPrefixScore;

    return best;
}

bool WssDecoder::injectTestSignal(
    Yuv444Frame& frame,
    Format format,
    int lineIndex)
{
    if (frame.width <= 0 ||
        frame.height <= 0 ||
        lineIndex < 0 ||
        lineIndex >= frame.height ||
        frame.sampleClockHz < 1'000'000.0 ||
        frame.y.size() <
            static_cast<std::size_t>(frame.width) *
            static_cast<std::size_t>(frame.height))
    {
        return false;
    }

    const double samplesPerElement =
        frame.sampleClockHz /
        kWssClockHz;

    const double requiredSamples =
        static_cast<double>(kTotalElementCount) *
        samplesPerElement;

    if (requiredSamples >=
        static_cast<double>(frame.width))
    {
        return false;
    }

    auto* line =
        frame.y.data() +
        static_cast<std::size_t>(lineIndex) *
        static_cast<std::size_t>(frame.width);

    // PAL 10-bit legal-range luma is stored left-shifted by six bits.
    // Nominal analogue levels used by the synthetic WSS source:
    //   black / WSS low = 0.300 V -> Y10 = 64
    //   WSS high         = 0.800 V -> Y10 ~= 689.714
    // The reconstructed waveform is deliberately not clamped; its normal
    // band-limited response may therefore show a small, realistic amount of
    // overshoot/undershoot around the shaped edges.
    constexpr std::uint16_t kBlackLevel = 64u << 6;
    constexpr std::uint16_t kHighLevel = 44'142u;

    // Pre-cooked sine-squared edge shape.  ETSI WSS is intentionally not a
    // hard-edged logic waveform; using a 200 ns (= one 5 MHz element) shaped
    // transition avoids injecting an unrealistic amount of HF energy at the
    // 13.5 MHz source sample rate.  Runtime work is only a table lookup.
    constexpr std::array<double, 17> kEdgeShape{
        0.000000,
        0.009607,
        0.038060,
        0.084265,
        0.146447,
        0.222215,
        0.308658,
        0.402455,
        0.500000,
        0.597545,
        0.691342,
        0.777785,
        0.853553,
        0.915735,
        0.961940,
        0.990393,
        1.000000
    };

    std::fill(
        line,
        line + frame.width,
        kBlackLevel);

    std::array<int, kDataBitCount> bits{};
    const int formatCode =
        static_cast<int>(format) & 0x7;

    bits[0] = formatCode & 1;
    bits[1] = (formatCode >> 1) & 1;
    bits[2] = (formatCode >> 2) & 1;

    // Group-1 uses odd parity over bits 0..3.
    bits[3] =
        ((bits[0] + bits[1] + bits[2]) & 1) == 0
        ? 1
        : 0;

    std::vector<int> elements;
    elements.reserve(kTotalElementCount);

    const auto& prefix = prefixPattern();
    elements.insert(
        elements.end(),
        prefix.begin(),
        prefix.end());

    for (int bit = 0;
         bit < kDataBitCount;
         ++bit)
    {
        // Decoder convention / ETSI bi-phase-L representation:
        //   0 = 000111
        //   1 = 111000
        for (int element = 0;
             element < kElementsPerDataBit;
             ++element)
        {
            const bool firstHalf =
                element < 3;
            const int value =
                bits[static_cast<std::size_t>(bit)] != 0
                ? (firstHalf ? 1 : 0)
                : (firstHalf ? 0 : 1);
            elements.push_back(value);
        }
    }

    // Put the synthetic burst near the beginning of the active line,
    // while leaving enough margin for interpolation at both ends.
    const double start =
        std::max(
            2.0,
            std::min(
                48.0,
                static_cast<double>(frame.width) -
                    requiredSamples - 2.0));

    const double edgeHalfWidth =
        samplesPerElement * 0.5;

    const auto levelAtElement =
        [&](int elementIndex) noexcept -> int
        {
            if (elementIndex < 0 ||
                elementIndex >=
                    static_cast<int>(elements.size()))
            {
                return 0;
            }
            return elements[static_cast<std::size_t>(elementIndex)];
        };

    const auto shapedFraction =
        [&](double phase) noexcept -> double
        {
            const double clamped =
                std::clamp(phase, 0.0, 1.0);
            const std::size_t index =
                static_cast<std::size_t>(
                    std::lround(
                        clamped *
                        static_cast<double>(kEdgeShape.size() - 1u)));
            return kEdgeShape[
                std::min(index, kEdgeShape.size() - 1u)];
        };

    for (int x = 0;
         x < frame.width;
         ++x)
    {
        const double samplePosition =
            static_cast<double>(x) + 0.5;
        const double relativeSamples =
            samplePosition - start;
        const double elementPosition =
            relativeSamples /
            samplesPerElement;
        const int currentElement =
            static_cast<int>(std::floor(elementPosition));

        if (currentElement < -1 ||
            currentElement >
                static_cast<int>(elements.size()))
        {
            continue;
        }

        int lowHigh = levelAtElement(currentElement);
        double amplitude =
            static_cast<double>(lowHigh);

        // Shape only actual logic transitions.  The transition occupies one
        // complete WSS clock element (200 ns), centred on the element boundary.
        const double leftBoundary =
            start +
            static_cast<double>(currentElement) *
                samplesPerElement;
        const int previousValue =
            levelAtElement(currentElement - 1);

        if (previousValue != lowHigh &&
            std::abs(samplePosition - leftBoundary) <= edgeHalfWidth)
        {
            const double phase =
                (samplePosition -
                 (leftBoundary - edgeHalfWidth)) /
                (2.0 * edgeHalfWidth);
            const double edge = shapedFraction(phase);
            amplitude =
                static_cast<double>(previousValue) +
                (static_cast<double>(lowHigh - previousValue) * edge);
        }
        else
        {
            const double rightBoundary =
                start +
                static_cast<double>(currentElement + 1) *
                    samplesPerElement;
            const int nextValue =
                levelAtElement(currentElement + 1);

            if (nextValue != lowHigh &&
                std::abs(samplePosition - rightBoundary) <= edgeHalfWidth)
            {
                const double phase =
                    (samplePosition -
                     (rightBoundary - edgeHalfWidth)) /
                    (2.0 * edgeHalfWidth);
                const double edge = shapedFraction(phase);
                amplitude =
                    static_cast<double>(lowHigh) +
                    (static_cast<double>(nextValue - lowHigh) * edge);
            }
        }

        const double y =
            static_cast<double>(kBlackLevel) +
            amplitude *
                static_cast<double>(
                    static_cast<int>(kHighLevel) -
                    static_cast<int>(kBlackLevel));

        line[x] =
            static_cast<std::uint16_t>(
                std::clamp(
                    std::lround(y),
                    0l,
                    65'535l));
    }

    return true;
}

std::string WssDecoder::formatDescription(
    Format format)
{
    switch (format)
    {
    case Format::Full4x3:
        return "4:3 full";
    case Format::Letterbox14x9Centre:
        return "14:9 letterbox centre";
    case Format::Letterbox14x9Top:
        return "14:9 letterbox top";
    case Format::Letterbox16x9Centre:
        return "16:9 letterbox centre";
    case Format::Letterbox16x9Top:
        return "16:9 letterbox top";
    case Format::LetterboxWiderCentre:
        return ">16:9 letterbox centre";
    case Format::Full4x3Protect14x9:
        return "4:3 full (14:9 protected)";
    case Format::Full16x9Anamorphic:
        return "16:9 anamorphic";
    case Format::None:
        break;
    }

    return "not detected";
}

int WssDecoder::recommendedDisplayAspect(
    Format format)
{
    // Letterboxed WSS modes are still transmitted inside a 4:3 raster.
    // Only full-format anamorphic WSS requires 16:9 display stretching.
    return
        format ==
            Format::Full16x9Anamorphic
        ? 1
        : 0;
}

WssDecoder::Result WssDecoder::process(
    const Yuv444Frame& frame)
{
    Candidate best;

    const int lineCount =
        std::min(
            frame.height,
            kSearchLineCount);

    if (frame.width > 0 &&
        frame.height > 0 &&
        frame.y.size() >=
            static_cast<std::size_t>(
                frame.width) *
            static_cast<std::size_t>(
                frame.height))
    {
        for (int lineIndex = 0;
             lineIndex < lineCount;
             ++lineIndex)
        {
            const Candidate candidate =
                decodeLine(
                    frame.y.data() +
                    static_cast<std::size_t>(
                        lineIndex) *
                    static_cast<std::size_t>(
                        frame.width),
                    frame.width,
                    frame.sampleClockHz,
                    lineIndex);

            if (candidate.valid &&
                (!best.valid ||
                 candidate.score >
                    best.score))
            {
                best = candidate;
            }
        }
    }

    if (best.valid)
    {
        missCount_ = 0;

        if (pendingFormatCode_ ==
            best.formatCode)
        {
            ++pendingCount_;
        }
        else
        {
            pendingFormatCode_ =
                best.formatCode;
            pendingCount_ = 1;
        }

        if (pendingCount_ >=
            kStableFramesRequired)
        {
            lockedFormatCode_ =
                pendingFormatCode_;
            lockedLine_ =
                best.line;
        }
    }
    else
    {
        pendingFormatCode_ = -1;
        pendingCount_ = 0;

        if (lockedFormatCode_ >= 0)
        {
            ++missCount_;

            if (missCount_ >=
                kUnlockMissFrames)
            {
                lockedFormatCode_ = -1;
                lockedLine_ = -1;
                missCount_ = 0;
            }
        }
    }

    Result result;
    result.validThisFrame = best.valid;
    result.detectedLine =
        best.valid ? best.line : -1;
    result.locked =
        lockedFormatCode_ >= 0;

    if (result.locked)
    {
        result.format =
            static_cast<Format>(
                lockedFormatCode_);

        result.displayBlankLine =
            best.valid
            ? best.line
            : lockedLine_;

        result.recommendedDisplayAspect =
            recommendedDisplayAspect(
                result.format);

        result.status =
            "WSS: " +
            formatDescription(
                result.format) +
            " - locked";
    }
    else if (best.valid)
    {
        result.format =
            static_cast<Format>(
                best.formatCode);
        result.displayBlankLine =
            best.line;
        result.status =
            "WSS: " +
            formatDescription(
                result.format) +
            " - acquiring";
    }
    else
    {
        result.status =
            "WSS: not detected";
    }

    const int publishedFormat =
        result.locked
        ? static_cast<int>(
            result.format)
        : -1;

    result.stateChanged =
        result.locked !=
            lastPublishedLocked_ ||
        publishedFormat !=
            lastPublishedFormatCode_;

    if (result.stateChanged)
    {
        lastPublishedLocked_ =
            result.locked;
        lastPublishedFormatCode_ =
            publishedFormat;
    }

    return result;
}
