#include "processing/LumaHighFrequencyCompensator.h"
#include "util/CpuFeatures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

    constexpr double kUnitEndpointAmplitude =
        1.1170782872165974;

    double correctionScaleForDb(double gainDb)
    {
        const double targetAmplitude =
            std::pow(10.0, gainDb / 20.0);

        return
            (targetAmplitude - 1.0) /
            (kUnitEndpointAmplitude - 1.0);
    }
}

void LumaHighFrequencyCompensator::process(
    Yuv444Frame& frame,
    int gainHundredthsDb)
{
    processRange(
        frame,
        gainHundredthsDb,
        0,
        frame.height);
}

void LumaHighFrequencyCompensator::processRange(
    Yuv444Frame& frame,
    int gainHundredthsDb,
    int firstLine,
    int lastLine)
{
    if (gainHundredthsDb <= 0 ||
        frame.width <= 2 * kRadius ||
        frame.height <= 0 ||
        frame.y.size() <
            static_cast<std::size_t>(frame.width) *
            static_cast<std::size_t>(frame.height))
    {
        return;
    }

    firstLine = std::clamp(firstLine, 0, frame.height);
    lastLine = std::clamp(lastLine, firstLine, frame.height);
    if (firstLine >= lastLine)
    {
        return;
    }

    const double gainDb =
        static_cast<double>(
            std::clamp(gainHundredthsDb, 0, 100)) /
        100.0;

    const double scale =
        correctionScaleForDb(gainDb);

    if (CpuFeatures::supportsAvx2Fma())
    {
        processAvx2Range(
            frame,
            scale,
            firstLine,
            lastLine);
        return;
    }

    // Local row copy makes independent line ranges safe to execute in
    // parallel on W0/W1/W2.  Every worker writes disjoint output rows.
    std::vector<std::uint16_t> lineBuffer(
        static_cast<std::size_t>(frame.width));

    for (int y = firstLine; y < lastLine; ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(frame.width);

        std::copy_n(
            frame.y.data() + lineOffset,
            frame.width,
            lineBuffer.data());

        for (int x = kRadius;
             x < frame.width - kRadius;
             ++x)
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
