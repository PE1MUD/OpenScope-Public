#include "SignalReconstructor.h"

#include <algorithm>
#include <immintrin.h>

void LineResampler::resampleAvx2(
    std::span<const float> input,
    std::span<float> output) const
{
    if (output.empty())
    {
        return;
    }

    if (input.empty())
    {
        std::fill(
            output.begin(),
            output.end(),
            0.0f);

        return;
    }

    if (input.size() == 1)
    {
        std::fill(
            output.begin(),
            output.end(),
            input.front());

        return;
    }

    if (cachedInputSize_ != input.size() ||
        cachedOutputSize_ != output.size())
    {
        rebuildCache(
            input.size(),
            output.size());
    }

    const int inputSize =
        static_cast<int>(input.size());

    const int tapCount =
        kernelRadius_ * 2;

    for (std::size_t outputIndex = 0;
        outputIndex < output.size();
        ++outputIndex)
    {
        const auto& cached =
            cache_[outputIndex];

        const int firstSample =
            cached.firstInputIndex;

        const int firstValidSample =
            std::max(
                firstSample,
                0);

        const int lastValidSample =
            std::min(
                firstSample + tapCount,
                inputSize);

        int sourceIndex =
            firstValidSample;

        __m256 vectorSum =
            _mm256_setzero_ps();

        while (sourceIndex + 8 <=
            lastValidSample)
        {
            const std::size_t inputOffset =
                static_cast<std::size_t>(
                    sourceIndex);

            const std::size_t weightOffset =
                cached.weightOffset +
                static_cast<std::size_t>(
                    sourceIndex -
                    firstSample);

            const __m256 samples =
                _mm256_loadu_ps(
                    input.data() +
                    inputOffset);

            const __m256 weights =
                _mm256_loadu_ps(
                    cachedWeights_.data() +
                    weightOffset);

            vectorSum =
                _mm256_fmadd_ps(
                    samples,
                    weights,
                    vectorSum);

            sourceIndex += 8;
        }

        alignas(32) float partialSums[8];

        _mm256_store_ps(
            partialSums,
            vectorSum);

        float weightedSum =
            partialSums[0] +
            partialSums[1] +
            partialSums[2] +
            partialSums[3] +
            partialSums[4] +
            partialSums[5] +
            partialSums[6] +
            partialSums[7];

        while (sourceIndex <
            lastValidSample)
        {
            const std::size_t weightOffset =
                cached.weightOffset +
                static_cast<std::size_t>(
                    sourceIndex -
                    firstSample);

            weightedSum +=
                input[
                    static_cast<std::size_t>(
                        sourceIndex)] *
                cachedWeights_[weightOffset];

                    ++sourceIndex;
        }

        output[outputIndex] =
            weightedSum *
            cached.inverseWeightSum;
    }
}