#include "VideoDeinterlacer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <immintrin.h>

namespace
{
    constexpr int kFirstFieldParity = 0;
    constexpr int kSecondFieldParity = 1;

    constexpr int kMotionThreshold = 1024;
    constexpr int kCombThreshold = 1536;
    constexpr int kMaskExpansion = 3;

    inline int average2(
        int a,
        int b) noexcept
    {
        return
            (a + b + 1) >>
            1;
    }

    int spatialPredict(
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int width,
        int x) noexcept
    {
        if (x < 2 ||
            x >= width - 2)
        {
            return
                (static_cast<int>(upperLine[x]) +
                    static_cast<int>(lowerLine[x]) +
                    1) >>
                1;
        }

        int bestScore =
            0x7fffffff;

        int bestDirection =
            0;

        for (int direction = -1;
            direction <= 1;
            ++direction)
        {
            int score = 0;

            for (int offset = -1;
                offset <= 1;
                ++offset)
            {
                const int upper =
                    static_cast<int>(
                        upperLine[
                            x +
                                offset -
                                direction]);

                const int lower =
                    static_cast<int>(
                        lowerLine[
                            x +
                                offset +
                                direction]);

                score +=
                    std::abs(
                        upper -
                        lower);
            }

            if (score < bestScore)
            {
                bestScore =
                    score;

                bestDirection =
                    direction;
            }
        }

        const int upper =
            static_cast<int>(
                upperLine[
                    x -
                        bestDirection]);

        const int lower =
            static_cast<int>(
                lowerLine[
                    x +
                        bestDirection]);

        return
            (upper +
                lower +
                1) >>
            1;
    }

    int temporalClamp(
        int spatialValue,
        int before,
        int after) noexcept
    {
        const int temporalCenter =
            average2(
                before,
                after);

        const int temporalDifference =
            std::abs(
                before -
                after);

        const int temporalLimit =
            (std::max)(
                temporalDifference >> 1,
                256);

        return std::clamp(
            spatialValue,
            temporalCenter -
            temporalLimit,
            temporalCenter +
            temporalLimit);
    }

    inline bool needsInterpolation(
        const std::uint16_t* beforeLine,
        const std::uint16_t* afterLine,
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int width,
        int x,
        bool weaveFromBefore) noexcept
    {
        int temporalDifference =
            0;

        int combDifference =
            0;

        const int firstX =
            (std::max)(
                0,
                x - 1);

        const int lastX =
            (std::min)(
                width - 1,
                x + 1);

        for (int testX = firstX;
            testX <= lastX;
            ++testX)
        {
            const int before =
                static_cast<int>(
                    beforeLine[testX]);

            const int after =
                static_cast<int>(
                    afterLine[testX]);

            temporalDifference +=
                std::abs(
                    before -
                    after);

            const int upper =
                static_cast<int>(
                    upperLine[testX]);

            const int lower =
                static_cast<int>(
                    lowerLine[testX]);

            const int verticalPrediction =
                average2(
                    upper,
                    lower);

            const int weave =
                weaveFromBefore
                ? before
                : after;

            combDifference +=
                std::abs(
                    weave -
                    verticalPrediction);
        }

        const int sampleCount =
            lastX -
            firstX +
            1;

        return
            temporalDifference >
            kMotionThreshold *
            sampleCount &&
            combDifference >
            kCombThreshold *
            sampleCount;
    }

    inline __m256i load8U16ToI32(
        const std::uint16_t* source) noexcept
    {
        const __m128i packed =
            _mm_loadu_si128(
                reinterpret_cast<
                const __m128i*>(
                    source));

        return
            _mm256_cvtepu16_epi32(
                packed);
    }

    inline __m256i absDifference32(
        __m256i a,
        __m256i b) noexcept
    {
        return
            _mm256_abs_epi32(
                _mm256_sub_epi32(
                    a,
                    b));
    }

    inline __m256i average2Vector(
        __m256i a,
        __m256i b) noexcept
    {
        return
            _mm256_srli_epi32(
                _mm256_add_epi32(
                    _mm256_add_epi32(
                        a,
                        b),
                    _mm256_set1_epi32(1)),
                1);
    }

    inline __m256i detectorMask8(
        const std::uint16_t* beforeLine,
        const std::uint16_t* afterLine,
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int x) noexcept
    {
        __m256i temporalSum =
            _mm256_setzero_si256();

        __m256i combSum =
            _mm256_setzero_si256();

        for (int offset = -1;
            offset <= 1;
            ++offset)
        {
            const __m256i before =
                load8U16ToI32(
                    beforeLine +
                    x +
                    offset);

            const __m256i after =
                load8U16ToI32(
                    afterLine +
                    x +
                    offset);

            temporalSum =
                _mm256_add_epi32(
                    temporalSum,
                    absDifference32(
                        before,
                        after));

            const __m256i upper =
                load8U16ToI32(
                    upperLine +
                    x +
                    offset);

            const __m256i lower =
                load8U16ToI32(
                    lowerLine +
                    x +
                    offset);

            const __m256i verticalPrediction =
                average2Vector(
                    upper,
                    lower);

            combSum =
                _mm256_add_epi32(
                    combSum,
                    absDifference32(
                        before,
                        verticalPrediction));
        }

        const __m256i motionThreshold =
            _mm256_set1_epi32(
                kMotionThreshold * 3);

        const __m256i combThreshold =
            _mm256_set1_epi32(
                kCombThreshold * 3);

        const __m256i motionMask =
            _mm256_cmpgt_epi32(
                temporalSum,
                motionThreshold);

        const __m256i combMask =
            _mm256_cmpgt_epi32(
                combSum,
                combThreshold);

        return
            _mm256_and_si256(
                motionMask,
                combMask);
    }

    inline __m256i expandedDetectorMask8(
        const std::uint16_t* beforeLine,
        const std::uint16_t* afterLine,
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int x) noexcept
    {
        __m256i mask =
            _mm256_setzero_si256();

        for (int offset = -kMaskExpansion;
            offset <= kMaskExpansion;
            ++offset)
        {
            mask =
                _mm256_or_si256(
                    mask,
                    detectorMask8(
                        beforeLine,
                        afterLine,
                        upperLine,
                        lowerLine,
                        x +
                        offset));
        }

        return mask;
    }

    inline __m256i spatialPredict8(
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int x) noexcept
    {
        __m256i scoreMinus1 =
            _mm256_setzero_si256();

        __m256i scoreZero =
            _mm256_setzero_si256();

        __m256i scorePlus1 =
            _mm256_setzero_si256();

        for (int offset = -1;
            offset <= 1;
            ++offset)
        {
            scoreMinus1 =
                _mm256_add_epi32(
                    scoreMinus1,
                    absDifference32(
                        load8U16ToI32(
                            upperLine +
                            x +
                            offset + 1),
                        load8U16ToI32(
                            lowerLine +
                            x +
                            offset - 1)));

            scoreZero =
                _mm256_add_epi32(
                    scoreZero,
                    absDifference32(
                        load8U16ToI32(
                            upperLine +
                            x +
                            offset),
                        load8U16ToI32(
                            lowerLine +
                            x +
                            offset)));

            scorePlus1 =
                _mm256_add_epi32(
                    scorePlus1,
                    absDifference32(
                        load8U16ToI32(
                            upperLine +
                            x +
                            offset - 1),
                        load8U16ToI32(
                            lowerLine +
                            x +
                            offset + 1)));
        }

        // Scalar tie behaviour is preserved:
        // direction -1 wins ties over 0, and 0 wins ties over +1.
        const __m256i zeroBeatsMinus1 =
            _mm256_cmpgt_epi32(
                scoreMinus1,
                scoreZero);

        __m256i bestScore =
            _mm256_blendv_epi8(
                scoreMinus1,
                scoreZero,
                zeroBeatsMinus1);

        __m256i bestUpper =
            _mm256_blendv_epi8(
                load8U16ToI32(
                    upperLine +
                    x + 1),
                load8U16ToI32(
                    upperLine +
                    x),
                zeroBeatsMinus1);

        __m256i bestLower =
            _mm256_blendv_epi8(
                load8U16ToI32(
                    lowerLine +
                    x - 1),
                load8U16ToI32(
                    lowerLine +
                    x),
                zeroBeatsMinus1);

        const __m256i plus1BeatsBest =
            _mm256_cmpgt_epi32(
                bestScore,
                scorePlus1);

        bestScore =
            _mm256_blendv_epi8(
                bestScore,
                scorePlus1,
                plus1BeatsBest);

        (void)bestScore;

        bestUpper =
            _mm256_blendv_epi8(
                bestUpper,
                load8U16ToI32(
                    upperLine +
                    x - 1),
                plus1BeatsBest);

        bestLower =
            _mm256_blendv_epi8(
                bestLower,
                load8U16ToI32(
                    lowerLine +
                    x + 1),
                plus1BeatsBest);

        return
            average2Vector(
                bestUpper,
                bestLower);
    }

    inline __m256i temporalClamp8(
        __m256i spatial,
        __m256i before,
        __m256i after) noexcept
    {
        const __m256i center =
            average2Vector(
                before,
                after);

        const __m256i difference =
            absDifference32(
                before,
                after);

        const __m256i limit =
            _mm256_max_epi32(
                _mm256_srli_epi32(
                    difference,
                    1),
                _mm256_set1_epi32(256));

        const __m256i minimum =
            _mm256_sub_epi32(
                center,
                limit);

        const __m256i maximum =
            _mm256_add_epi32(
                center,
                limit);

        return
            _mm256_min_epi32(
                _mm256_max_epi32(
                    spatial,
                    minimum),
                maximum);
    }

    inline void store8U16(
        std::uint16_t* destination,
        __m256i values) noexcept
    {
        values =
            _mm256_max_epi32(
                values,
                _mm256_setzero_si256());

        values =
            _mm256_min_epi32(
                values,
                _mm256_set1_epi32(65535));

        const __m128i low =
            _mm256_castsi256_si128(
                values);

        const __m128i high =
            _mm256_extracti128_si256(
                values,
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

    void processInterpolatedLineScalarPixel(
        const std::uint16_t* beforeLine,
        const std::uint16_t* afterLine,
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int width,
        int x,
        std::uint16_t* destinationLine) noexcept
    {
        bool interpolate =
            false;

        const int firstTestX =
            (std::max)(
                0,
                x - kMaskExpansion);

        const int lastTestX =
            (std::min)(
                width - 1,
                x + kMaskExpansion);

        for (int testX = firstTestX;
            testX <= lastTestX;
            ++testX)
        {
            if (needsInterpolation(
                beforeLine,
                afterLine,
                upperLine,
                lowerLine,
                width,
                testX,
                true))
            {
                interpolate =
                    true;

                break;
            }
        }

        if (!interpolate)
        {
            return;
        }

        const int spatial =
            spatialPredict(
                upperLine,
                lowerLine,
                width,
                x);

        const int before =
            static_cast<int>(
                beforeLine[x]);

        const int after =
            static_cast<int>(
                afterLine[x]);

        const int output =
            temporalClamp(
                spatial,
                before,
                after);

        destinationLine[x] =
            static_cast<std::uint16_t>(
                std::clamp(
                    output,
                    0,
                    65535));
    }

    void processInterpolatedLineAvx2(
        const std::uint16_t* beforeLine,
        const std::uint16_t* afterLine,
        const std::uint16_t* upperLine,
        const std::uint16_t* lowerLine,
        int width,
        std::uint16_t* destinationLine) noexcept
    {
        // Expansion is +/-3 and the detector itself touches +/-1.
        // Keep four pixels scalar on both sides so all AVX2 loads are in-range.
        constexpr int kVectorBorder =
            kMaskExpansion + 1;

        int x = 0;

        const int scalarPrefixEnd =
            (std::min)(
                width,
                kVectorBorder);

        for (;
            x < scalarPrefixEnd;
            ++x)
        {
            processInterpolatedLineScalarPixel(
                beforeLine,
                afterLine,
                upperLine,
                lowerLine,
                width,
                x,
                destinationLine);
        }

        const int vectorEnd =
            width -
            kVectorBorder;

        for (;
            x + 7 < vectorEnd;
            x += 8)
        {
            const __m256i interpolationMask =
                expandedDetectorMask8(
                    beforeLine,
                    afterLine,
                    upperLine,
                    lowerLine,
                    x);

            if (_mm256_testz_si256(
                interpolationMask,
                interpolationMask))
            {
                continue;
            }

            const __m256i spatial =
                spatialPredict8(
                    upperLine,
                    lowerLine,
                    x);

            const __m256i before =
                load8U16ToI32(
                    beforeLine +
                    x);

            const __m256i after =
                load8U16ToI32(
                    afterLine +
                    x);

            const __m256i interpolated =
                temporalClamp8(
                    spatial,
                    before,
                    after);

            const __m256i baseline =
                load8U16ToI32(
                    destinationLine +
                    x);

            const __m256i output =
                _mm256_blendv_epi8(
                    baseline,
                    interpolated,
                    interpolationMask);

            store8U16(
                destinationLine +
                x,
                output);
        }

        for (;
            x < width;
            ++x)
        {
            processInterpolatedLineScalarPixel(
                beforeLine,
                afterLine,
                upperLine,
                lowerLine,
                width,
                x,
                destinationLine);
        }
    }
}

bool VideoDeinterlacer::beginFrame(
    const std::uint16_t* source,
    int width,
    int height,
    ProgressiveLumaPair& destination)
{
    if (source == nullptr ||
        width <= 0 ||
        height <= 0)
    {
        return false;
    }

    currentSource_ = source;
    currentDestination_ = &destination;
    currentWidth_ = width;
    currentHeight_ = height;
    currentSampleCount_ =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    destination.first.resize(
        width,
        height);

    destination.second.resize(
        width,
        height);

    if (!hasPreviousFrame_ ||
        previousLuma_.size() != currentSampleCount_)
    {
        currentMode_ =
            FrameMode::First;
    }
    else if (previousPreviousLuma_.size() !=
        currentSampleCount_)
    {
        currentMode_ =
            FrameMode::Second;
    }
    else
    {
        currentMode_ =
            FrameMode::Normal;
    }

    return true;
}

void VideoDeinterlacer::processRange(
    int firstLine,
    int lastLine)
{
    if (currentSource_ == nullptr ||
        currentDestination_ == nullptr ||
        currentWidth_ <= 0 ||
        currentHeight_ <= 0)
    {
        return;
    }

    firstLine =
        std::clamp(
            firstLine,
            0,
            currentHeight_);

    lastLine =
        std::clamp(
            lastLine,
            firstLine,
            currentHeight_);

    const std::size_t width =
        static_cast<std::size_t>(
            currentWidth_);

    if (currentMode_ ==
        FrameMode::First)
    {
        for (int y = firstLine;
            y < lastLine;
            ++y)
        {
            const std::size_t lineOffset =
                static_cast<std::size_t>(y) *
                width;

            std::copy_n(
                currentSource_ +
                lineOffset,
                width,
                currentDestination_->
                first.y.data() +
                lineOffset);

            std::copy_n(
                currentSource_ +
                lineOffset,
                width,
                currentDestination_->
                second.y.data() +
                lineOffset);
        }

        return;
    }

    if (currentMode_ ==
        FrameMode::Second)
    {
        for (int y = firstLine;
            y < lastLine;
            ++y)
        {
            const std::size_t lineOffset =
                static_cast<std::size_t>(y) *
                width;

            std::copy_n(
                previousLuma_.data() +
                lineOffset,
                width,
                currentDestination_->
                first.y.data() +
                lineOffset);

            std::copy_n(
                previousLuma_.data() +
                lineOffset,
                width,
                currentDestination_->
                second.y.data() +
                lineOffset);
        }

        return;
    }

    const std::uint16_t* older =
        previousPreviousLuma_.data();

    const std::uint16_t* center =
        previousLuma_.data();

    const std::uint16_t* newer =
        currentSource_;

    for (int y = firstLine;
        y < lastLine;
        ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            width;

        std::uint16_t* firstDestinationLine =
            currentDestination_->
            first.y.data() +
            lineOffset;

        if ((y & 1) ==
            kFirstFieldParity)
        {
            std::copy_n(
                center +
                lineOffset,
                width,
                firstDestinationLine);
        }
        else
        {
            const std::uint16_t* beforeLine =
                older +
                lineOffset;

            std::copy_n(
                beforeLine,
                width,
                firstDestinationLine);

            if (y > 0 &&
                y < currentHeight_ - 1)
            {
                const std::uint16_t* afterLine =
                    center +
                    lineOffset;

                const std::uint16_t* upperLine =
                    center +
                    static_cast<std::size_t>(
                        y - 1) *
                    width;

                const std::uint16_t* lowerLine =
                    center +
                    static_cast<std::size_t>(
                        y + 1) *
                    width;

                processInterpolatedLineAvx2(
                    beforeLine,
                    afterLine,
                    upperLine,
                    lowerLine,
                    currentWidth_,
                    firstDestinationLine);
            }
        }

        std::uint16_t* secondDestinationLine =
            currentDestination_->
            second.y.data() +
            lineOffset;

        std::copy_n(
            center +
            lineOffset,
            width,
            secondDestinationLine);

        if (y <= 0 ||
            y >= currentHeight_ - 1 ||
            (y & 1) !=
            kFirstFieldParity)
        {
            continue;
        }

        const std::uint16_t* beforeLine =
            center +
            lineOffset;

        const std::uint16_t* afterLine =
            newer +
            lineOffset;

        const std::uint16_t* upperLine =
            center +
            static_cast<std::size_t>(
                y - 1) *
            width;

        const std::uint16_t* lowerLine =
            center +
            static_cast<std::size_t>(
                y + 1) *
            width;

        processInterpolatedLineAvx2(
            beforeLine,
            afterLine,
            upperLine,
            lowerLine,
            currentWidth_,
            secondDestinationLine);
    }
}

void VideoDeinterlacer::endFrame()
{
    if (currentSource_ == nullptr ||
        currentSampleCount_ == 0)
    {
        return;
    }

    if (currentMode_ ==
        FrameMode::First)
    {
        previousLuma_.assign(
            currentSource_,
            currentSource_ +
            currentSampleCount_);

        previousPreviousLuma_.clear();
        hasPreviousFrame_ = true;
    }
    else if (currentMode_ ==
        FrameMode::Second)
    {
        previousPreviousLuma_ =
            previousLuma_;

        previousLuma_.assign(
            currentSource_,
            currentSource_ +
            currentSampleCount_);
    }
    else
    {
        previousPreviousLuma_.swap(
            previousLuma_);

        previousLuma_.assign(
            currentSource_,
            currentSource_ +
            currentSampleCount_);
    }

    currentSource_ = nullptr;
    currentDestination_ = nullptr;
    currentWidth_ = 0;
    currentHeight_ = 0;
    currentSampleCount_ = 0;
}

void VideoDeinterlacer::deinterlace(
    const std::uint16_t* source,
    int width,
    int height,
    ProgressiveLumaPair& destination)
{
    if (!beginFrame(
        source,
        width,
        height,
        destination))
    {
        return;
    }

    processRange(
        0,
        height);

    endFrame();
}