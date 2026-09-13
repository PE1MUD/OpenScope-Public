#include "processing/LumaHighFrequencyCompensator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include <vector>

namespace
{
    constexpr int kRadius = 8;

    constexpr std::array<double, 17> kUnitCorrectionKernel =
    {
        -0.00043087478646947205,
        -0.0000003169517475910403,
        -0.0007501379827647017,
        -0.00023109977423583537,
        -0.0010725169719288056,
        -0.0019790519694750787,
        -0.0013128860859289422,
        -0.027304513969740495,
         1.066162796984582,
        -0.027304513969740495,
        -0.0013128860859289422,
        -0.0019790519694750787,
        -0.0010725169719288056,
        -0.00023109977423583537,
        -0.0007501379827647017,
        -0.0000003169517475910403,
        -0.00043087478646947205
    };

    inline __m256d load4U16AsDouble(
        const std::uint16_t* source) noexcept
    {
        const __m128i packed =
            _mm_loadl_epi64(
                reinterpret_cast<const __m128i*>(source));

        const __m128i values32 =
            _mm_cvtepu16_epi32(packed);

        return _mm256_cvtepi32_pd(values32);
    }
}

void LumaHighFrequencyCompensator::processAvx2Range(
    Yuv444Frame& frame,
    double scale,
    int firstLine,
    int lastLine)
{
    std::vector<std::uint16_t> lineBuffer(
        static_cast<std::size_t>(frame.width));

    const __m256d centerCorrectionCoefficient =
        _mm256_set1_pd(
            kUnitCorrectionKernel[kRadius] - 1.0);

    std::array<__m256d, kRadius> tapCoefficients{};

    for (int tap = 1; tap <= kRadius; ++tap)
    {
        tapCoefficients[static_cast<std::size_t>(tap - 1)] =
            _mm256_set1_pd(
                kUnitCorrectionKernel[
                    static_cast<std::size_t>(kRadius - tap)]);
    }

    const __m256d scaleVector =
        _mm256_set1_pd(scale);

    for (int y = firstLine; y < lastLine; ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(frame.width);

        std::copy_n(
            frame.y.data() + lineOffset,
            frame.width,
            lineBuffer.data());

        int x = kRadius;
        const int vectorEnd =
            frame.width - kRadius - 3;

        for (; x < vectorEnd; x += 4)
        {
            const std::uint16_t* centerSource =
                lineBuffer.data() +
                static_cast<std::size_t>(x);

            const __m256d center =
                load4U16AsDouble(centerSource);

            __m256d correction =
                _mm256_mul_pd(
                    center,
                    centerCorrectionCoefficient);

            for (int tap = 1; tap <= kRadius; ++tap)
            {
                const __m256d left =
                    load4U16AsDouble(
                        centerSource - tap);

                const __m256d right =
                    load4U16AsDouble(
                        centerSource + tap);

                const __m256d pair =
                    _mm256_add_pd(left, right);

                correction =
                    _mm256_add_pd(
                        correction,
                        _mm256_mul_pd(
                            pair,
                            tapCoefficients[
                                static_cast<std::size_t>(tap - 1)]));
            }

            const __m256d value =
                _mm256_add_pd(
                    center,
                    _mm256_mul_pd(
                        scaleVector,
                        correction));

            alignas(32) double values[4];
            _mm256_store_pd(values, value);

            for (int lane = 0; lane < 4; ++lane)
            {
                frame.y[
                    lineOffset +
                    static_cast<std::size_t>(x + lane)] =
                    static_cast<std::uint16_t>(
                        std::clamp(
                            std::lround(values[lane]),
                            0L,
                            65535L));
            }
        }

        // Scalar tail keeps the exact original arithmetic and edge policy.
        for (; x < frame.width - kRadius; ++x)
        {
            double value =
                static_cast<double>(
                    lineBuffer[static_cast<std::size_t>(x)]);

            double correction =
                (kUnitCorrectionKernel[kRadius] - 1.0) *
                static_cast<double>(
                    lineBuffer[static_cast<std::size_t>(x)]);

            for (int tap = 1; tap <= kRadius; ++tap)
            {
                const double coefficient =
                    kUnitCorrectionKernel[
                        static_cast<std::size_t>(kRadius - tap)];

                correction +=
                    coefficient *
                    (static_cast<double>(
                        lineBuffer[static_cast<std::size_t>(x - tap)]) +
                     static_cast<double>(
                        lineBuffer[static_cast<std::size_t>(x + tap)]));
            }

            value += scale * correction;

            frame.y[
                lineOffset +
                static_cast<std::size_t>(x)] =
                static_cast<std::uint16_t>(
                    std::clamp(
                        std::lround(value),
                        0L,
                        65535L));
        }
    }
}
