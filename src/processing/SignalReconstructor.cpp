#include "SignalReconstructor.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <QElapsedTimer>

LineResampler::LineResampler(
    int kernelRadius,
    float cutoff)
    : kernelRadius_(std::max(kernelRadius, 1))
    , cutoff_(std::clamp(cutoff, 0.01f, 1.0f))
{}

void LineResampler::setImplementation(
    ResamplerImplementation implementation) noexcept
{
    implementation_ =
        implementation;
}

void LineResampler::rebuildCache(
    std::size_t inputSize,
    std::size_t outputSize) const
{
    cachedInputSize_ = inputSize;
    cachedOutputSize_ = outputSize;
    ++cacheGeneration_;

    cache_.clear();
    cache_.resize(outputSize);
    
    const std::size_t tapCount =
        static_cast<std::size_t>(
            kernelRadius_ * 2);

    cachedWeights_.clear();
    cachedWeights_.resize(
        outputSize * tapCount);

    const float scale =
        static_cast<float>(outputSize) /
        static_cast<float>(inputSize);

    const float effectiveCutoff = cutoff_;

    for (std::size_t outputIndex = 0;
        outputIndex < outputSize;
        ++outputIndex)
    {
        const float sourcePosition =
            (static_cast<float>(outputIndex) + 0.5f) /
            scale - 0.5f;

        const int centre =
            static_cast<int>(std::floor(sourcePosition));

        const int firstSample =
            centre - kernelRadius_ + 1;

        auto& sample =
            cache_[outputIndex];

        sample.firstInputIndex =
            firstSample;

        sample.weightOffset =
            outputIndex * tapCount;

        for (int tap = 0;
            tap < kernelRadius_ * 2;
            ++tap)
        {
            const int sourceIndex =
                firstSample + tap;

            const float distance =
                sourcePosition -
                static_cast<float>(sourceIndex);

            cachedWeights_[
                sample.weightOffset +
                    static_cast<std::size_t>(tap)] =
                kernel(
                    distance,
                    effectiveCutoff);
        }
        float weightSum = 0.0f;

        for (int tap = 0;
            tap < kernelRadius_ * 2;
            ++tap)
        {
            const int sourceIndex =
                firstSample + tap;

            if (sourceIndex < 0 ||
                sourceIndex >= static_cast<int>(inputSize))
            {
                continue;
            }

            weightSum +=
                cachedWeights_[
                    sample.weightOffset +
                        static_cast<std::size_t>(tap)];
        }

        sample.inverseWeightSum =
            std::abs(weightSum) > 1.0e-8f
            ? 1.0f / weightSum
            : 0.0f;
    }
}

void LineResampler::resample(
    std::span<const float> input,
    std::span<float> output) const
{
    switch (implementation_)
    {
    case ResamplerImplementation::Avx2:
        resampleAvx2(
            input,
            output);
        break;

    case ResamplerImplementation::Scalar:
    default:
        resampleScalar(
            input,
            output);
        break;
    }
}

void LineResampler::resampleScalar(
    std::span<const float> input,
    std::span<float> output) const
{
    if (output.empty()) {
        return;
    }

    if (input.empty()) {
        std::fill(output.begin(), output.end(), 0.0f);
        return;
    }

    if (input.size() == 1) {
        std::fill(output.begin(), output.end(), input.front());
        return;
    }

    if (cachedInputSize_ != input.size() ||
        cachedOutputSize_ != output.size())
    {
        rebuildCache(
            input.size(),
            output.size());
    }

    for (std::size_t outputIndex = 0;
        outputIndex < output.size();
        ++outputIndex)
    {
        float weightedSum = 0.0f;

        const auto& cached =
            cache_[outputIndex];

        const int firstSample =
            cached.firstInputIndex;

        const int lastSample =
            firstSample + kernelRadius_ * 2 - 1;

        for (int sourceIndex = firstSample;
            sourceIndex <= lastSample;
            ++sourceIndex) {
            if (sourceIndex < 0 ||
                sourceIndex >= static_cast<int>(input.size()))
            {
                continue;
            }

            const float weight =
                cachedWeights_[
                    cached.weightOffset +
                        static_cast<std::size_t>(
                            sourceIndex -
                            cached.firstInputIndex)];

            weightedSum +=
                input[static_cast<std::size_t>(sourceIndex)] *
                weight;
        }

        output[outputIndex] =
            weightedSum *
            cached.inverseWeightSum;
    }
}

float LineResampler::sinc(float x)
{
    if (std::abs(x) < 1.0e-6f) {
        return 1.0f;
    }

    const float piX =
        std::numbers::pi_v<float> *x;

    return std::sin(piX) / piX;
}

float LineResampler::kernel(
    float distance,
    float cutoff) const
{
    const float absoluteDistance =
        std::abs(distance);

    if (absoluteDistance >=
        static_cast<float>(kernelRadius_)) {
        return 0.0f;
    }

    // Blackman window over the finite sinc kernel.
    const float normalisedDistance =
        absoluteDistance /
        static_cast<float>(kernelRadius_);

    const float window =
        0.42f
        + 0.5f * std::cos(
            std::numbers::pi_v<float> *
            normalisedDistance)
        + 0.08f * std::cos(
            2.0f *
            std::numbers::pi_v<float> *
            normalisedDistance);

    return cutoff *
        sinc(cutoff * distance) *
        window;
}