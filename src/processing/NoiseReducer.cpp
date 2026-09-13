#include "NoiseReducer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace
{
    inline std::uint32_t absoluteDifference(
        std::uint16_t first,
        std::uint16_t second) noexcept
    {
        return first >= second
            ? static_cast<std::uint32_t>(
                first - second)
            : static_cast<std::uint32_t>(
                second - first);
    }

    inline __m256i load8Unsigned16As32(
        const std::uint16_t* source) noexcept
    {
        const __m128i packed =
            _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(
                    source));

        return
            _mm256_cvtepu16_epi32(
                packed);
    }

    inline void accumulateNeighbour(
        __m256i neighbour,
        __m256i center,
        __m256i threshold,
        __m256i one,
        __m256i& sum,
        __m256i& count) noexcept
    {
        const __m256i difference =
            _mm256_abs_epi32(
                _mm256_sub_epi32(
                    neighbour,
                    center));

        const __m256i greaterThanThreshold =
            _mm256_cmpgt_epi32(
                difference,
                threshold);

        const __m256i acceptedMask =
            _mm256_xor_si256(
                greaterThanThreshold,
                _mm256_set1_epi32(-1));

        sum =
            _mm256_add_epi32(
                sum,
                _mm256_and_si256(
                    neighbour,
                    acceptedMask));

        count =
            _mm256_add_epi32(
                count,
                _mm256_and_si256(
                    one,
                    acceptedMask));
    }

    inline void storeRoundedAverage(
        __m256i sum,
        __m256i count,
        std::uint16_t* destination) noexcept
    {
        // Exact integer rule of the scalar implementation:
        //
        //     (sum + count / 2) / count
        //
        // All values here are small enough to be represented exactly as
        // float integers. Truncation after the division therefore gives
        // the same positive integer rounding rule.
        const __m256i numerator =
            _mm256_add_epi32(
                sum,
                _mm256_srli_epi32(
                    count,
                    1));

        const __m256 quotient =
            _mm256_div_ps(
                _mm256_cvtepi32_ps(
                    numerator),
                _mm256_cvtepi32_ps(
                    count));

        const __m256i values32 =
            _mm256_cvttps_epi32(
                quotient);

        const __m128i low =
            _mm256_castsi256_si128(
                values32);

        const __m128i high =
            _mm256_extracti128_si256(
                values32,
                1);

        const __m128i packed =
            _mm_packus_epi32(
                low,
                high);

        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(
                destination),
            packed);
    }
}


void NoiseReducer::process(
    const Yuv444Frame& source,
    Yuv444Frame& destination,
    int intensity)
{
    if (source.width <= 0 ||
        source.height <= 0 ||
        source.y.empty() ||
        source.u.empty() ||
        source.v.empty())
    {
        return;
    }

    if (destination.width != source.width ||
        destination.height != source.height)
    {
        destination.resize(
            source.width,
            source.height);
    }

    destination.sampleClockHz = source.sampleClockHz;

    processRange(
        source,
        destination,
        intensity,
        0,
        source.height);
}

void NoiseReducer::processRange(
    const Yuv444Frame& source,
    Yuv444Frame& destination,
    int intensity,
    int firstLine,
    int lastLine) const
{
    if (source.width <= 0 ||
        source.height <= 0 ||
        source.y.empty() ||
        source.u.empty() ||
        source.v.empty() ||
        destination.width != source.width ||
        destination.height != source.height)
    {
        return;
    }

    firstLine = std::clamp(
        firstLine,
        0,
        source.height);

    lastLine = std::clamp(
        lastLine,
        firstLine,
        source.height);

    if (firstLine >= lastLine)
    {
        return;
    }

    const int clampedIntensity =
        std::clamp(
            intensity,
            kMinimumIntensity,
            kMaximumIntensity);

    if (clampedIntensity == 0)
    {
        const std::size_t firstIndex =
            static_cast<std::size_t>(firstLine) *
            static_cast<std::size_t>(source.width);

        const std::size_t sampleCount =
            static_cast<std::size_t>(lastLine - firstLine) *
            static_cast<std::size_t>(source.width);

        std::copy_n(
            source.y.begin() +
                static_cast<std::ptrdiff_t>(firstIndex),
            sampleCount,
            destination.y.begin() +
                static_cast<std::ptrdiff_t>(firstIndex));

        std::copy_n(
            source.u.begin() +
                static_cast<std::ptrdiff_t>(firstIndex),
            sampleCount,
            destination.u.begin() +
                static_cast<std::ptrdiff_t>(firstIndex));

        std::copy_n(
            source.v.begin() +
                static_cast<std::ptrdiff_t>(firstIndex),
            sampleCount,
            destination.v.begin() +
                static_cast<std::ptrdiff_t>(firstIndex));

        return;
    }

    // Preserve the earlier behaviour around intensity 50, while
    // allowing the upper half of the slider to become substantially
    // more aggressive.
    const int thresholdPosition =
        std::clamp(
            clampedIntensity * 2 - 100,
            0,
            100);

    const auto interpolateThreshold =
        [thresholdPosition](
            std::uint16_t minimumThreshold,
            std::uint16_t maximumThreshold)
        {
            const std::uint32_t minimum =
                minimumThreshold;

            const std::uint32_t range =
                static_cast<std::uint32_t>(
                    maximumThreshold) -
                minimum;

            return static_cast<std::uint16_t>(
                minimum +
                (range *
                    static_cast<std::uint32_t>(
                        thresholdPosition) +
                    50u) /
                100u);
        };

    const std::uint16_t lumaThreshold =
        interpolateThreshold(
            kMinimumLumaThreshold,
            kMaximumLumaThreshold);

    const std::uint16_t chromaThreshold =
        interpolateThreshold(
            kMinimumChromaThreshold,
            kMaximumChromaThreshold);

    filterPlaneRange(
        source.y,
        destination.y,
        source.width,
        source.height,
        lumaThreshold,
        firstLine,
        lastLine);

    filterPlaneRange(
        source.u,
        destination.u,
        source.width,
        source.height,
        chromaThreshold,
        firstLine,
        lastLine);

    filterPlaneRange(
        source.v,
        destination.v,
        source.width,
        source.height,
        chromaThreshold,
        firstLine,
        lastLine);

    if (clampedIntensity < kMaximumIntensity)
    {
        blendPlaneRange(
            source.y,
            destination.y,
            source.width,
            source.height,
            clampedIntensity,
            firstLine,
            lastLine);

        blendPlaneRange(
            source.u,
            destination.u,
            source.width,
            source.height,
            clampedIntensity,
            firstLine,
            lastLine);

        blendPlaneRange(
            source.v,
            destination.v,
            source.width,
            source.height,
            clampedIntensity,
            firstLine,
            lastLine);
    }
}

void NoiseReducer::filterPlaneRange(
    const std::vector<std::uint16_t>& source,
    std::vector<std::uint16_t>& destination,
    int width,
    int height,
    std::uint16_t threshold,
    int firstLine,
    int lastLine)
{
    const std::size_t expectedSize =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    if (source.size() < expectedSize ||
        destination.size() < expectedSize)
    {
        return;
    }

    firstLine = std::clamp(
        firstLine,
        0,
        height);

    lastLine = std::clamp(
        lastLine,
        firstLine,
        height);

    if (firstLine >= lastLine)
    {
        return;
    }

    if (width < 5 ||
        height < 5)
    {
        const std::size_t firstIndex =
            static_cast<std::size_t>(firstLine) *
            static_cast<std::size_t>(width);

        const std::size_t sampleCount =
            static_cast<std::size_t>(lastLine - firstLine) *
            static_cast<std::size_t>(width);

        std::copy_n(
            source.begin() +
                static_cast<std::ptrdiff_t>(firstIndex),
            sampleCount,
            destination.begin() +
                static_cast<std::ptrdiff_t>(firstIndex));
        return;
    }

    const __m256i thresholdVector =
        _mm256_set1_epi32(
            static_cast<int>(
                threshold));

    const __m256i one =
        _mm256_set1_epi32(1);

    for (int y = firstLine;
        y < lastLine;
        ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width);

        // Vertical border is copied unchanged. This also makes line-range
        // splitting safe because no worker needs to write halo lines.
        if (y < kFilterRadius ||
            y >= height - kFilterRadius)
        {
            std::copy_n(
                source.begin() +
                    static_cast<std::ptrdiff_t>(lineOffset),
                static_cast<std::size_t>(width),
                destination.begin() +
                    static_cast<std::ptrdiff_t>(lineOffset));
            continue;
        }

        for (int borderX = 0;
            borderX < kFilterRadius;
            ++borderX)
        {
            destination[
                lineOffset +
                    static_cast<std::size_t>(
                        borderX)] =
                source[
                    lineOffset +
                        static_cast<std::size_t>(
                            borderX)];

            destination[
                lineOffset +
                    static_cast<std::size_t>(
                        width - 1 - borderX)] =
                source[
                    lineOffset +
                        static_cast<std::size_t>(
                            width - 1 - borderX)];
        }

        int x = kFilterRadius;

        const int vectorEnd =
            width -
            kFilterRadius -
            7;

        for (;
            x < vectorEnd;
            x += 8)
        {
            const std::size_t centerIndex =
                lineOffset +
                static_cast<std::size_t>(x);

            const __m256i center =
                load8Unsigned16As32(
                    source.data() +
                    centerIndex);

            __m256i sum =
                _mm256_setzero_si256();

            __m256i count =
                _mm256_setzero_si256();

            for (int offsetY = -kFilterRadius;
                offsetY <= kFilterRadius;
                ++offsetY)
            {
                const std::size_t neighbourLine =
                    static_cast<std::size_t>(
                        y + offsetY) *
                    static_cast<std::size_t>(
                        width);

                for (int offsetX = -kFilterRadius;
                    offsetX <= kFilterRadius;
                    ++offsetX)
                {
                    const std::size_t neighbourIndex =
                        neighbourLine +
                        static_cast<std::size_t>(
                            x + offsetX);

                    const __m256i neighbour =
                        load8Unsigned16As32(
                            source.data() +
                            neighbourIndex);

                    accumulateNeighbour(
                        neighbour,
                        center,
                        thresholdVector,
                        one,
                        sum,
                        count);
                }
            }

            storeRoundedAverage(
                sum,
                count,
                destination.data() +
                centerIndex);
        }

        for (;
            x < width - kFilterRadius;
            ++x)
        {
            const std::size_t centerIndex =
                lineOffset +
                static_cast<std::size_t>(x);

            const std::uint16_t center =
                source[centerIndex];

            std::uint32_t sum = 0;
            std::uint32_t count = 0;

            for (int offsetY = -kFilterRadius;
                offsetY <= kFilterRadius;
                ++offsetY)
            {
                const std::size_t neighbourLine =
                    static_cast<std::size_t>(
                        y + offsetY) *
                    static_cast<std::size_t>(
                        width);

                for (int offsetX = -kFilterRadius;
                    offsetX <= kFilterRadius;
                    ++offsetX)
                {
                    const std::uint16_t neighbour =
                        source[
                            neighbourLine +
                                static_cast<std::size_t>(
                                    x + offsetX)];

                    if (absoluteDifference(
                        neighbour,
                        center) <= threshold)
                    {
                        sum += neighbour;
                        ++count;
                    }
                }
            }

            destination[centerIndex] =
                static_cast<std::uint16_t>(
                    (sum + count / 2u) /
                    count);
        }
    }
}

void NoiseReducer::blendPlaneRange(
    const std::vector<std::uint16_t>& source,
    std::vector<std::uint16_t>& filtered,
    int width,
    int height,
    int intensity,
    int firstLine,
    int lastLine)
{
    const std::size_t expectedSize =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    if (source.size() < expectedSize ||
        filtered.size() < expectedSize)
    {
        return;
    }

    firstLine = std::clamp(
        firstLine,
        0,
        height);

    lastLine = std::clamp(
        lastLine,
        firstLine,
        height);

    if (firstLine >= lastLine)
    {
        return;
    }

    const int clampedIntensity =
        std::clamp(
            intensity,
            kMinimumIntensity,
            kMaximumIntensity);

    const std::size_t firstIndex =
        static_cast<std::size_t>(firstLine) *
        static_cast<std::size_t>(width);

    const std::size_t lastIndex =
        static_cast<std::size_t>(lastLine) *
        static_cast<std::size_t>(width);

    if (clampedIntensity <= 0)
    {
        std::copy(
            source.begin() +
                static_cast<std::ptrdiff_t>(firstIndex),
            source.begin() +
                static_cast<std::ptrdiff_t>(lastIndex),
            filtered.begin() +
                static_cast<std::ptrdiff_t>(firstIndex));
        return;
    }

    if (clampedIntensity >= kMaximumIntensity)
    {
        return;
    }

    const __m256i strength =
        _mm256_set1_epi32(
            clampedIntensity);

    const __m256i inverseStrength =
        _mm256_set1_epi32(
            kMaximumIntensity -
            clampedIntensity);

    const __m256i rounding =
        _mm256_set1_epi32(50);

    std::size_t index = firstIndex;

    const std::size_t vectorEnd =
        lastIndex -
        ((lastIndex - firstIndex) % 8u);

    for (;
        index < vectorEnd;
        index += 8)
    {
        const __m128i source16 =
            _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(
                    source.data() + index));

        const __m128i filtered16 =
            _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(
                    filtered.data() + index));

        const __m256i source32 =
            _mm256_cvtepu16_epi32(
                source16);

        const __m256i filtered32 =
            _mm256_cvtepu16_epi32(
                filtered16);

        __m256i blended =
            _mm256_add_epi32(
                _mm256_mullo_epi32(
                    source32,
                    inverseStrength),
                _mm256_mullo_epi32(
                    filtered32,
                    strength));

        blended =
            _mm256_add_epi32(
                blended,
                rounding);

        // Exact integer division by 100 using scalar lanes would be costly.
        // The original implementation used float for this final blend; keep
        // the same numerical behaviour here while restricting it to the range.
        const __m256 blendedFloat =
            _mm256_mul_ps(
                _mm256_cvtepi32_ps(blended),
                _mm256_set1_ps(0.01f));

        const __m256i rounded =
            _mm256_cvttps_epi32(
                blendedFloat);

        const __m128i low =
            _mm256_castsi256_si128(
                rounded);

        const __m128i high =
            _mm256_extracti128_si256(
                rounded,
                1);

        const __m128i packed =
            _mm_packus_epi32(
                low,
                high);

        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(
                filtered.data() + index),
            packed);
    }

    for (;
        index < lastIndex;
        ++index)
    {
        const std::uint32_t sourceValue =
            source[index];

        const std::uint32_t filteredValue =
            filtered[index];

        filtered[index] =
            static_cast<std::uint16_t>(
                (sourceValue *
                    static_cast<std::uint32_t>(
                        kMaximumIntensity -
                        clampedIntensity) +
                    filteredValue *
                    static_cast<std::uint32_t>(
                        clampedIntensity) +
                    50u) /
                100u);
    }
}
