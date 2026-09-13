#include "DisplayConverter.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <QtGlobal>
#include <QElapsedTimer>
#include <immintrin.h>

DisplayConverter::DisplayConverter()
{
    rebuildDisplayLut();
    rebuildColorConversionLuts();
}

void DisplayConverter::rebuildColorConversionLuts()
{
    for (int i = 0;
        i < 256;
        ++i)
    {
        const int y =
            i - 16;

        const int chroma =
            i - 128;

        yToC_[static_cast<std::size_t>(i)] =
            298 * y;

        vToRed_[static_cast<std::size_t>(i)] =
            409 * chroma;

        uToGreen_[static_cast<std::size_t>(i)] =
            -100 * chroma;

        vToGreen_[static_cast<std::size_t>(i)] =
            -208 * chroma;

        uToBlue_[static_cast<std::size_t>(i)] =
            516 * chroma;
    }
}

void DisplayConverter::setHighlightedLine(int line)
{
    highlightedLine_ = line;
}

namespace
{
    inline __m256i clampInt32ToByteRange(
        __m256i value)
    {
        const __m256i zero =
            _mm256_setzero_si256();

        const __m256i maximum =
            _mm256_set1_epi32(255);

        value =
            _mm256_max_epi32(
                value,
                zero);

        value =
            _mm256_min_epi32(
                value,
                maximum);

        return value;
    }
}

QImage DisplayConverter::convert(
    const Yuv444Frame& frame,
    const std::uint16_t* luma,
    int outputWidth,
    int outputHeight,
    DisplayPerformance& performance) const
{
    switch (implementation_)
    {
    case DisplayConversionImplementation::Avx2:
        return convertAvx2(
            frame,
            luma,
            nullptr,
            0,
            outputWidth,
            outputHeight,
            0,
            outputHeight,
            performance);

    case DisplayConversionImplementation::Scalar:
    default:
        return convertScalar(
            frame,
            luma,
            nullptr,
            0,
            outputWidth,
            outputHeight,
            0,
            outputHeight,
            performance);
    }
}

void DisplayConverter::convertRange(
    const Yuv444Frame& frame,
    const std::uint16_t* luma,
    QRgb* outputPixels,
    int outputStridePixels,
    int outputWidth,
    int outputHeight,
    int firstOutputY,
    int lastOutputY,
    DisplayPerformance& performance) const
{
    if (outputPixels == nullptr ||
        outputStridePixels < outputWidth)
    {
        performance = {};
        return;
    }

    switch (implementation_)
    {
    case DisplayConversionImplementation::Avx2:
        convertAvx2(
            frame,
            luma,
            outputPixels,
            outputStridePixels,
            outputWidth,
            outputHeight,
            firstOutputY,
            lastOutputY,
            performance);
        break;

    case DisplayConversionImplementation::Scalar:
    default:
        convertScalar(
            frame,
            luma,
            outputPixels,
            outputStridePixels,
            outputWidth,
            outputHeight,
            firstOutputY,
            lastOutputY,
            performance);
        break;
    }
}

bool DisplayConverter::convertNativeRange(
    const Yuv444Frame& frame,
    const std::uint16_t* luma,
    QRgb* outputPixels,
    int outputStridePixels,
    int firstOutputY,
    int lastOutputY,
    DisplayPerformance& performance) const
{
    performance = {};

    if (implementation_ !=
            DisplayConversionImplementation::Avx2 ||
        luma == nullptr ||
        outputPixels == nullptr ||
        outputStridePixels < frame.width ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.u.empty() ||
        frame.v.empty() ||
        displayGamma_ != 1.0 ||
        highlightedLine_ >= 0 ||
        highlightedEndX_ >= 0)
    {
        return false;
    }

    firstOutputY = std::clamp(
        firstOutputY,
        0,
        frame.height);

    lastOutputY = std::clamp(
        lastOutputY,
        firstOutputY,
        frame.height);

    if (firstOutputY >= lastOutputY)
    {
        return true;
    }

    const std::size_t sampleCount =
        static_cast<std::size_t>(frame.width) *
        static_cast<std::size_t>(frame.height);

    if (frame.u.size() < sampleCount ||
        frame.v.size() < sampleCount)
    {
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    const __m256i yOffset =
        _mm256_set1_epi32(16);

    const __m256i chromaOffset =
        _mm256_set1_epi32(128);

    const __m256i coefficientY =
        _mm256_set1_epi32(298);

    const __m256i coefficientRV =
        _mm256_set1_epi32(409);

    const __m256i coefficientGU =
        _mm256_set1_epi32(100);

    const __m256i coefficientGV =
        _mm256_set1_epi32(208);

    const __m256i coefficientBU =
        _mm256_set1_epi32(516);

    const __m256i rounding =
        _mm256_set1_epi32(128);

    const __m256i alpha =
        _mm256_set1_epi32(
            static_cast<int>(0xff000000u));

    performance.setupUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    for (int y = firstOutputY;
        y < lastOutputY;
        ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(frame.width);

        const std::uint16_t* yy =
            luma + lineOffset;

        const std::uint16_t* u =
            frame.u.data() + lineOffset;

        const std::uint16_t* v =
            frame.v.data() + lineOffset;

        QRgb* dst =
            outputPixels +
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(outputStridePixels);

        int x = 0;

        for (;
            x + 7 < frame.width;
            x += 8)
        {
            const __m128i y16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        yy + x));

            const __m128i u16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        u + x));

            const __m128i v16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        v + x));

            __m256i y32 =
                _mm256_cvtepu16_epi32(y16);

            __m256i u32 =
                _mm256_cvtepu16_epi32(u16);

            __m256i v32 =
                _mm256_cvtepu16_epi32(v16);

            y32 = _mm256_sub_epi32(
                _mm256_srli_epi32(y32, 8),
                yOffset);

            u32 = _mm256_sub_epi32(
                _mm256_srli_epi32(u32, 8),
                chromaOffset);

            v32 = _mm256_sub_epi32(
                _mm256_srli_epi32(v32, 8),
                chromaOffset);

            const __m256i c =
                _mm256_mullo_epi32(
                    y32,
                    coefficientY);

            __m256i red =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        _mm256_add_epi32(
                            c,
                            _mm256_mullo_epi32(
                                v32,
                                coefficientRV)),
                        rounding),
                    8);

            __m256i green =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        _mm256_sub_epi32(
                            _mm256_sub_epi32(
                                c,
                                _mm256_mullo_epi32(
                                    u32,
                                    coefficientGU)),
                            _mm256_mullo_epi32(
                                v32,
                                coefficientGV)),
                        rounding),
                    8);

            __m256i blue =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        _mm256_add_epi32(
                            c,
                            _mm256_mullo_epi32(
                                u32,
                                coefficientBU)),
                        rounding),
                    8);

            red = clampInt32ToByteRange(red);
            green = clampInt32ToByteRange(green);
            blue = clampInt32ToByteRange(blue);

            __m256i pixels = alpha;

            pixels = _mm256_or_si256(
                pixels,
                _mm256_slli_epi32(
                    red,
                    16));

            pixels = _mm256_or_si256(
                pixels,
                _mm256_slli_epi32(
                    green,
                    8));

            pixels = _mm256_or_si256(
                pixels,
                blue);

            _mm256_storeu_si256(
                reinterpret_cast<__m256i*>(
                    dst + x),
                pixels);
        }

        for (;
            x < frame.width;
            ++x)
        {
            const int y8 =
                static_cast<int>(yy[x] >> 8) - 16;

            const int u8 =
                static_cast<int>(u[x] >> 8) - 128;

            const int v8 =
                static_cast<int>(v[x] >> 8) - 128;

            const int c = 298 * y8;

            const int r = std::clamp(
                (c + 409 * v8 + 128) >> 8,
                0,
                255);

            const int g = std::clamp(
                (c - 100 * u8 - 208 * v8 + 128) >> 8,
                0,
                255);

            const int b = std::clamp(
                (c + 516 * u8 + 128) >> 8,
                0,
                255);

            dst[x] = qRgb(r, g, b);
        }
    }

    performance.colorConversionUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    return true;
}

bool DisplayConverter::convertNativePair(
    const Yuv444Frame& frame,
    const std::uint16_t* firstLuma,
    const std::uint16_t* secondLuma,
    QImage& firstImage,
    QImage& secondImage,
    DisplayPerformance& performance) const
{
    performance = {};

    // This path is deliberately narrow. It is for the fixed 720x576
    // video Spout path where no scaling/highlight/gamma operation is
    // required. Falling back keeps every other display use unchanged.
    if (implementation_ !=
            DisplayConversionImplementation::Avx2 ||
        firstLuma == nullptr ||
        secondLuma == nullptr ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.u.empty() ||
        frame.v.empty() ||
        displayGamma_ != 1.0 ||
        highlightedLine_ >= 0 ||
        highlightedEndX_ >= 0)
    {
        return false;
    }

    const std::size_t sampleCount =
        static_cast<std::size_t>(frame.width) *
        static_cast<std::size_t>(frame.height);

    if (frame.u.size() < sampleCount ||
        frame.v.size() < sampleCount)
    {
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    firstImage = QImage(
        frame.width,
        frame.height,
        QImage::Format_RGB32);

    secondImage = QImage(
        frame.width,
        frame.height,
        QImage::Format_RGB32);

    if (firstImage.isNull() ||
        secondImage.isNull())
    {
        firstImage = {};
        secondImage = {};
        return false;
    }

    performance.allocationUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    const __m256i yOffset =
        _mm256_set1_epi32(16);

    const __m256i chromaOffset =
        _mm256_set1_epi32(128);

    const __m256i coefficientY =
        _mm256_set1_epi32(298);

    const __m256i coefficientRV =
        _mm256_set1_epi32(409);

    const __m256i coefficientGU =
        _mm256_set1_epi32(100);

    const __m256i coefficientGV =
        _mm256_set1_epi32(208);

    const __m256i coefficientBU =
        _mm256_set1_epi32(516);

    const __m256i rounding =
        _mm256_set1_epi32(128);

    const __m256i alpha =
        _mm256_set1_epi32(
            static_cast<int>(0xff000000u));

    performance.setupUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    for (int y = 0;
        y < frame.height;
        ++y)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(frame.width);

        const std::uint16_t* y1 =
            firstLuma + lineOffset;

        const std::uint16_t* y2 =
            secondLuma + lineOffset;

        const std::uint16_t* u =
            frame.u.data() + lineOffset;

        const std::uint16_t* v =
            frame.v.data() + lineOffset;

        auto* dst1 =
            reinterpret_cast<QRgb*>(
                firstImage.scanLine(y));

        auto* dst2 =
            reinterpret_cast<QRgb*>(
                secondImage.scanLine(y));

        int x = 0;

        for (;
            x + 7 < frame.width;
            x += 8)
        {
            const __m128i y1_16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        y1 + x));

            const __m128i y2_16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        y2 + x));

            const __m128i u16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        u + x));

            const __m128i v16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        v + x));

            __m256i yy1 =
                _mm256_cvtepu16_epi32(y1_16);

            __m256i yy2 =
                _mm256_cvtepu16_epi32(y2_16);

            __m256i uu =
                _mm256_cvtepu16_epi32(u16);

            __m256i vv =
                _mm256_cvtepu16_epi32(v16);

            yy1 = _mm256_sub_epi32(
                _mm256_srli_epi32(yy1, 8),
                yOffset);

            yy2 = _mm256_sub_epi32(
                _mm256_srli_epi32(yy2, 8),
                yOffset);

            uu = _mm256_sub_epi32(
                _mm256_srli_epi32(uu, 8),
                chromaOffset);

            vv = _mm256_sub_epi32(
                _mm256_srli_epi32(vv, 8),
                chromaOffset);

            // U/V are identical for both deinterlaced outputs. Do the
            // chroma arithmetic once and reuse it for both field images.
            const __m256i rv =
                _mm256_mullo_epi32(
                    vv,
                    coefficientRV);

            const __m256i gu =
                _mm256_mullo_epi32(
                    uu,
                    coefficientGU);

            const __m256i gv =
                _mm256_mullo_epi32(
                    vv,
                    coefficientGV);

            const __m256i bu =
                _mm256_mullo_epi32(
                    uu,
                    coefficientBU);

            const auto makePixels =
                [&](const __m256i yy)
                {
                    const __m256i c =
                        _mm256_mullo_epi32(
                            yy,
                            coefficientY);

                    __m256i red =
                        _mm256_srai_epi32(
                            _mm256_add_epi32(
                                _mm256_add_epi32(
                                    c,
                                    rv),
                                rounding),
                            8);

                    __m256i green =
                        _mm256_srai_epi32(
                            _mm256_add_epi32(
                                _mm256_sub_epi32(
                                    _mm256_sub_epi32(
                                        c,
                                        gu),
                                    gv),
                                rounding),
                            8);

                    __m256i blue =
                        _mm256_srai_epi32(
                            _mm256_add_epi32(
                                _mm256_add_epi32(
                                    c,
                                    bu),
                                rounding),
                            8);

                    red = clampInt32ToByteRange(red);
                    green = clampInt32ToByteRange(green);
                    blue = clampInt32ToByteRange(blue);

                    __m256i pixels = alpha;

                    pixels = _mm256_or_si256(
                        pixels,
                        _mm256_slli_epi32(
                            red,
                            16));

                    pixels = _mm256_or_si256(
                        pixels,
                        _mm256_slli_epi32(
                            green,
                            8));

                    pixels = _mm256_or_si256(
                        pixels,
                        blue);

                    return pixels;
                };

            const __m256i pixels1 =
                makePixels(yy1);

            const __m256i pixels2 =
                makePixels(yy2);

            _mm256_storeu_si256(
                reinterpret_cast<__m256i*>(
                    dst1 + x),
                pixels1);

            _mm256_storeu_si256(
                reinterpret_cast<__m256i*>(
                    dst2 + x),
                pixels2);
        }

        for (;
            x < frame.width;
            ++x)
        {
            const int yy1 =
                static_cast<int>(y1[x] >> 8) - 16;

            const int yy2 =
                static_cast<int>(y2[x] >> 8) - 16;

            const int uu =
                static_cast<int>(u[x] >> 8) - 128;

            const int vv =
                static_cast<int>(v[x] >> 8) - 128;

            const int rv = 409 * vv;
            const int gu = 100 * uu;
            const int gv = 208 * vv;
            const int bu = 516 * uu;

            const auto makePixel =
                [&](int yy)
                {
                    const int c = 298 * yy;

                    const int r = std::clamp(
                        (c + rv + 128) >> 8,
                        0,
                        255);

                    const int g = std::clamp(
                        (c - gu - gv + 128) >> 8,
                        0,
                        255);

                    const int b = std::clamp(
                        (c + bu + 128) >> 8,
                        0,
                        255);

                    return qRgb(r, g, b);
                };

            dst1[x] = makePixel(yy1);
            dst2[x] = makePixel(yy2);
        }
    }

    performance.colorConversionUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    return true;
}

QImage DisplayConverter::convertScalar(
    const Yuv444Frame& frame,
    const std::uint16_t* luma,
    QRgb* outputPixels,
    int outputStridePixels,
    int outputWidth,
    int outputHeight,
    int firstOutputY,
    int lastOutputY,
    DisplayPerformance& performance) const
{
    performance = {};

    if (frame.width <= 0 ||
        frame.height <= 0 ||
        outputWidth <= 0 ||
        outputHeight <= 0 ||
        frame.y.empty() ||
        frame.u.empty() ||
        frame.v.empty())
    {
        return {};
    }

    QElapsedTimer timer;
    timer.start();

    QImage image;

    if (outputPixels == nullptr)
    {
        image =
            QImage(
                outputWidth,
                outputHeight,
                QImage::Format_RGB32);

        outputPixels =
            reinterpret_cast<QRgb*>(
                image.bits());

        outputStridePixels =
            image.bytesPerLine() /
            static_cast<int>(
                sizeof(QRgb));
    }

    firstOutputY =
        std::clamp(
            firstOutputY,
            0,
            outputHeight);

    lastOutputY =
        std::clamp(
            lastOutputY,
            firstOutputY,
            outputHeight);

    performance.allocationUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    const float horizontalScale =
        static_cast<float>(frame.width) /
        static_cast<float>(outputWidth);

    if (cachedInputWidth_ != frame.width ||
        cachedOutputWidth_ != outputWidth)
    {
        horizontalLeftIndex_.resize(
            static_cast<std::size_t>(outputWidth));

        horizontalRightIndex_.resize(
            static_cast<std::size_t>(outputWidth));

        horizontalFraction_.resize(
            static_cast<std::size_t>(outputWidth));

        for (int outputX = 0;
            outputX < outputWidth;
            ++outputX)
        {
            const float sourcePosition =
                (static_cast<float>(outputX) + 0.5f) *
                horizontalScale -
                0.5f;

            const int leftIndex =
                std::clamp(
                    static_cast<int>(
                        std::floor(sourcePosition)),
                    0,
                    frame.width - 1);

            const int rightIndex =
                std::min(
                    leftIndex + 1,
                    frame.width - 1);

            const float fraction =
                std::clamp(
                    sourcePosition -
                    static_cast<float>(leftIndex),
                    0.0f,
                    1.0f);

            const std::size_t index =
                static_cast<std::size_t>(outputX);

            horizontalLeftIndex_[index] =
                leftIndex;

            horizontalRightIndex_[index] =
                rightIndex;

            horizontalFraction_[index] =
                fraction;
        }

        cachedInputWidth_ =
            frame.width;

        cachedOutputWidth_ =
            outputWidth;
    }

    const float verticalScale =
        static_cast<float>(frame.height) /
        static_cast<float>(outputHeight);

    const bool limitHighlightX =
        highlightedEndX_ >= 0;

    const int highlightedOutputStartX =
        limitHighlightX
        ? std::clamp(
            highlightedStartX_ *
            outputWidth /
            frame.width,
            0,
            outputWidth)
        : 0;

    const int highlightedOutputEndX =
        limitHighlightX
        ? std::clamp(
            highlightedEndX_ *
            outputWidth /
            frame.width,
            0,
            outputWidth)
        : outputWidth;

    performance.setupUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    for (int outputY = firstOutputY;
        outputY < lastOutputY;
        ++outputY)
    {
        auto* dst =
            outputPixels +
            static_cast<std::size_t>(
                outputY) *
            static_cast<std::size_t>(
                outputStridePixels);

        const float sourceLinePosition =
            (static_cast<float>(outputY) + 0.5f) *
            verticalScale -
            0.5f;

        const int sourceLine =
            std::clamp(
                static_cast<int>(
                    std::round(sourceLinePosition)),
                0,
                frame.height - 1);

        const std::size_t lineOffset =
            static_cast<std::size_t>(sourceLine) *
            static_cast<std::size_t>(frame.width);

        const auto* srcY =
            luma +
            lineOffset;

        const auto* srcU =
            frame.u.data() +
            lineOffset;

        const auto* srcV =
            frame.v.data() +
            lineOffset;

        const int highlightedOutputY =
            static_cast<int>(
                std::round(
                    (
                        static_cast<double>(
                            highlightedLine_) +
                        0.5
                        ) *
                    static_cast<double>(outputHeight) /
                    static_cast<double>(frame.height) -
                    0.5));

        const bool invertLine =
            highlightedLine_ >= 0 &&
            outputY >= highlightedOutputY &&
            outputY < highlightedOutputY + 2;

        for (int outputX = 0;
            outputX < outputWidth;
            ++outputX)
        {
            const std::size_t index =
                static_cast<std::size_t>(outputX);

            const int sourceIndex =
                horizontalLeftIndex_[index];

            const float sourceY =
                static_cast<float>(
                    srcY[sourceIndex]);

            const float sourceU =
                static_cast<float>(
                    srcU[sourceIndex]);

            const float sourceV =
                static_cast<float>(
                    srcV[sourceIndex]);

            const int y8 =
                std::clamp(
                    static_cast<int>(
                        sourceY / 256.0f),
                    0,
                    255);

            const int u8 =
                std::clamp(
                    static_cast<int>(
                        sourceU / 256.0f),
                    0,
                    255);

            const int v8 =
                std::clamp(
                    static_cast<int>(
                        sourceV / 256.0f),
                    0,
                    255);

            const int c =
                yToC_[
                    static_cast<std::size_t>(y8)];

            const int r =
                (c +
                    vToRed_[
                        static_cast<std::size_t>(v8)] +
                    128) >> 8;

            const int g =
                (c +
                    uToGreen_[
                        static_cast<std::size_t>(u8)] +
                    vToGreen_[
                        static_cast<std::size_t>(v8)] +
                            128) >> 8;

            const int b =
                (c +
                    uToBlue_[
                        static_cast<std::size_t>(u8)] +
                    128) >> 8;

            int outR =
                qBound(
                    0,
                    r,
                    255);

            int outG =
                qBound(
                    0,
                    g,
                    255);

            int outB =
                qBound(
                    0,
                    b,
                    255);

            outR =
                displayLut_[
                    static_cast<std::size_t>(outR)];

            outG =
                displayLut_[
                    static_cast<std::size_t>(outG)];

            outB =
                displayLut_[
                    static_cast<std::size_t>(outB)];

            if (invertLine)
            {
                outR =
                    255 - outR;

                outG =
                    255 - outG;

                outB =
                    255 - outB;
            }

            dst[outputX] =
                qRgb(
                    outR,
                    outG,
                    outB);
        }
    }

    performance.composeUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    return image;
}
void DisplayConverter::setGamma(double gamma)
{
    displayGamma_ = gamma;

    rebuildDisplayLut();
}

void DisplayConverter::rebuildDisplayLut()
{
    const double gamma =
        displayGamma_;

    for (std::size_t i = 0;
        i < displayLut_.size();
        ++i)
    {
        const double input =
            static_cast<double>(i) /
            255.0;

        const double output =
            std::pow(
                input,
                gamma);

        displayLut_[i] =
            static_cast<std::uint8_t>(
                std::clamp(
                    std::lround(
                        output * 255.0),
                    0l,
                    255l));
        displayLut32_[i] =
            static_cast<int>(
                displayLut_[i]);
    }
}

void DisplayConverter::setImplementation(
    DisplayConversionImplementation implementation)
{
    implementation_ =
        implementation;
}
QImage DisplayConverter::convertAvx2(
    const Yuv444Frame& frame,
    const std::uint16_t* luma,
    QRgb* outputPixels,
    int outputStridePixels,
    int outputWidth,
    int outputHeight,
    int firstOutputY,
    int lastOutputY,
    DisplayPerformance& performance) const
{
    performance = {};

    if (frame.width <= 0 ||
        frame.height <= 0 ||
        outputWidth <= 0 ||
        outputHeight <= 0 ||
        luma == nullptr ||
        frame.u.empty() ||
        frame.v.empty() ||
        frame.y.empty())
    {
        return {};
    }

    QElapsedTimer timer;
    timer.start();

    QImage image;

    if (outputPixels == nullptr)
    {
        image =
            QImage(
                outputWidth,
                outputHeight,
                QImage::Format_RGB32);

        outputPixels =
            reinterpret_cast<QRgb*>(
                image.bits());

        outputStridePixels =
            image.bytesPerLine() /
            static_cast<int>(
                sizeof(QRgb));
    }

    firstOutputY =
        std::clamp(
            firstOutputY,
            0,
            outputHeight);

    lastOutputY =
        std::clamp(
            lastOutputY,
            firstOutputY,
            outputHeight);

    performance.allocationUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    const float horizontalScale =
        static_cast<float>(frame.width) /
        static_cast<float>(outputWidth);

    if (cachedInputWidth_ != frame.width ||
        cachedOutputWidth_ != outputWidth)
    {
        horizontalLeftIndex_.resize(
            static_cast<std::size_t>(
                outputWidth));

        horizontalRightIndex_.resize(
            static_cast<std::size_t>(
                outputWidth));

        horizontalFraction_.resize(
            static_cast<std::size_t>(
                outputWidth));

        for (int outputX = 0;
            outputX < outputWidth;
            ++outputX)
        {
            const float sourcePosition =
                (static_cast<float>(outputX) + 0.5f) *
                horizontalScale -
                0.5f;

            const int leftIndex =
                std::clamp(
                    static_cast<int>(
                        std::floor(
                            sourcePosition)),
                    0,
                    frame.width - 1);

            const int rightIndex =
                (std::min)(
                    leftIndex + 1,
                    frame.width - 1);

            const float fraction =
                std::clamp(
                    sourcePosition -
                    static_cast<float>(
                        leftIndex),
                    0.0f,
                    1.0f);

            const std::size_t index =
                static_cast<std::size_t>(
                    outputX);

            horizontalLeftIndex_[index] =
                leftIndex;

            horizontalRightIndex_[index] =
                rightIndex;

            horizontalFraction_[index] =
                fraction;
        }

        cachedInputWidth_ =
            frame.width;

        cachedOutputWidth_ =
            outputWidth;
    }

    for (auto& line : resampledYLines_)
    {
        line.resize(
            static_cast<std::size_t>(
                outputWidth));
    }

    for (auto& line : resampledULines_)
    {
        line.resize(
            static_cast<std::size_t>(
                outputWidth));
    }

    for (auto& line : resampledVLines_)
    {
        line.resize(
            static_cast<std::size_t>(
                outputWidth));
    }

    std::array<int, 4> cachedYLineNumbers{
        -1, -1, -1, -1
    };

    std::array<int, 2> cachedULineNumbers{
        -1, -1
    };

    std::array<int, 2> cachedVLineNumbers{
        -1, -1
    };

    std::size_t nextYCacheSlot = 0;
    std::size_t nextUCacheSlot = 0;
    std::size_t nextVCacheSlot = 0;

    const auto horizontalResample =
        [this, outputWidth](
            const std::uint16_t* source,
            std::vector<float>& destination)
        {
            for (int x = 0;
                x < outputWidth;
                ++x)
            {
                const std::size_t index =
                    static_cast<std::size_t>(x);

                const int leftIndex =
                    horizontalLeftIndex_[index];

                const int rightIndex =
                    horizontalRightIndex_[index];

                const float fraction =
                    horizontalFraction_[index];

                const float left =
                    static_cast<float>(
                        source[leftIndex]);

                const float right =
                    static_cast<float>(
                        source[rightIndex]);

                destination[index] =
                    left +
                    (right - left) *
                    fraction;
            }
        };

    const auto getYLine =
        [&](int lineNumber) -> const float*
        {
            for (std::size_t slot = 0;
                slot < cachedYLineNumbers.size();
                ++slot)
            {
                if (cachedYLineNumbers[slot] ==
                    lineNumber)
                {
                    return
                        resampledYLines_[slot].data();
                }
            }

            const std::size_t slot =
                nextYCacheSlot;

            nextYCacheSlot =
                (nextYCacheSlot + 1) %
                resampledYLines_.size();

            const std::size_t lineOffset =
                static_cast<std::size_t>(
                    lineNumber) *
                static_cast<std::size_t>(
                    frame.width);

            horizontalResample(
                luma + lineOffset,
                resampledYLines_[slot]);

            cachedYLineNumbers[slot] =
                lineNumber;

            return
                resampledYLines_[slot].data();
        };

    const auto getULine =
        [&](int lineNumber) -> const float*
        {
            for (std::size_t slot = 0;
                slot < cachedULineNumbers.size();
                ++slot)
            {
                if (cachedULineNumbers[slot] ==
                    lineNumber)
                {
                    return
                        resampledULines_[slot].data();
                }
            }

            const std::size_t slot =
                nextUCacheSlot;

            nextUCacheSlot =
                (nextUCacheSlot + 1) %
                resampledULines_.size();

            const std::size_t lineOffset =
                static_cast<std::size_t>(
                    lineNumber) *
                static_cast<std::size_t>(
                    frame.width);

            horizontalResample(
                frame.u.data() +
                lineOffset,
                resampledULines_[slot]);

            cachedULineNumbers[slot] =
                lineNumber;

            return
                resampledULines_[slot].data();
        };

    const auto getVLine =
        [&](int lineNumber) -> const float*
        {
            for (std::size_t slot = 0;
                slot < cachedVLineNumbers.size();
                ++slot)
            {
                if (cachedVLineNumbers[slot] ==
                    lineNumber)
                {
                    return
                        resampledVLines_[slot].data();
                }
            }

            const std::size_t slot =
                nextVCacheSlot;

            nextVCacheSlot =
                (nextVCacheSlot + 1) %
                resampledVLines_.size();

            const std::size_t lineOffset =
                static_cast<std::size_t>(
                    lineNumber) *
                static_cast<std::size_t>(
                    frame.width);

            horizontalResample(
                frame.v.data() +
                lineOffset,
                resampledVLines_[slot]);

            cachedVLineNumbers[slot] =
                lineNumber;

            return
                resampledVLines_[slot].data();
        };

    const float verticalScale =
        static_cast<float>(frame.height) /
        static_cast<float>(outputHeight);

    const bool limitHighlightX =
        highlightedEndX_ >= 0;

    const int highlightedOutputStartX =
        limitHighlightX
        ? std::clamp(
            highlightedStartX_ *
            outputWidth /
            frame.width,
            0,
            outputWidth)
        : 0;

    const int highlightedOutputEndX =
        limitHighlightX
        ? std::clamp(
            highlightedEndX_ *
            outputWidth /
            frame.width,
            0,
            outputWidth)
        : outputWidth;

    const int highlightedOutputY =
        highlightedLine_ >= 0
        ? static_cast<int>(
            std::round(
                (
                    static_cast<double>(
                        highlightedLine_) +
                    0.5
                    ) *
                static_cast<double>(
                    outputHeight) /
                static_cast<double>(
                    frame.height) -
                0.5))
        : -1;

    performance.setupUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    const __m256 scaleTo8Bit =
        _mm256_set1_ps(
            1.0f / 256.0f);

    const __m256 half =
        _mm256_set1_ps(
            0.5f);

    const __m256 two =
        _mm256_set1_ps(
            2.0f);

    const __m256 three =
        _mm256_set1_ps(
            3.0f);

    const __m256 four =
        _mm256_set1_ps(
            4.0f);

    const __m256 five =
        _mm256_set1_ps(
            5.0f);

    const __m256i yOffset =
        _mm256_set1_epi32(16);

    const __m256i chromaOffset =
        _mm256_set1_epi32(128);

    const __m256i coefficientY =
        _mm256_set1_epi32(298);

    const __m256i coefficientRV =
        _mm256_set1_epi32(409);

    const __m256i coefficientGU =
        _mm256_set1_epi32(100);

    const __m256i coefficientGV =
        _mm256_set1_epi32(208);

    const __m256i coefficientBU =
        _mm256_set1_epi32(516);

    const __m256i rounding =
        _mm256_set1_epi32(128);

    const __m256i maximum =
        _mm256_set1_epi32(255);

    const __m256i alpha =
        _mm256_set1_epi32(
            static_cast<int>(
                0xff000000u));

    for (int outputY = firstOutputY;
        outputY < lastOutputY;
        ++outputY)
    {
        auto* dst =
            outputPixels +
            static_cast<std::size_t>(
                outputY) *
            static_cast<std::size_t>(
                outputStridePixels);

        const float sourceLinePosition =
            (static_cast<float>(outputY) + 0.5f) *
            verticalScale -
            0.5f;

        const int topLine =
            std::clamp(
                static_cast<int>(
                    std::floor(
                        sourceLinePosition)),
                0,
                frame.height - 1);

        const int bottomLine =
            (std::min)(
                topLine + 1,
                frame.height - 1);

        const int previousLine =
            (std::max)(
                topLine - 1,
                0);

        const int nextLine =
            (std::min)(
                bottomLine + 1,
                frame.height - 1);

        const float verticalFraction =
            std::clamp(
                sourceLinePosition -
                static_cast<float>(
                    topLine),
                0.0f,
                1.0f);

        const float t =
            verticalFraction;

        const float t2 =
            t * t;

        const float t3 =
            t2 * t;

        const __m256 tVector =
            _mm256_set1_ps(t);

        const __m256 t2Vector =
            _mm256_set1_ps(t2);

        const __m256 t3Vector =
            _mm256_set1_ps(t3);

        const __m256 chromaVerticalFraction =
            _mm256_set1_ps(
                verticalFraction);

        const float* yPreviousLine =
            getYLine(
                previousLine);

        const float* yTopLine =
            getYLine(
                topLine);

        const float* yBottomLine =
            getYLine(
                bottomLine);

        const float* yNextLine =
            getYLine(
                nextLine);

        const float* uTopLine =
            getULine(
                topLine);

        const float* uBottomLine =
            getULine(
                bottomLine);

        const float* vTopLine =
            getVLine(
                topLine);

        const float* vBottomLine =
            getVLine(
                bottomLine);

        const bool highlightThisLine =
            highlightedLine_ >= 0 &&
            outputY >= highlightedOutputY &&
            outputY < highlightedOutputY + 2;

        int outputX = 0;

        for (;
            outputX + 7 < outputWidth;
            outputX += 8)
        {
            const __m256 yPrevious =
                _mm256_loadu_ps(
                    yPreviousLine +
                    outputX);

            const __m256 yTop =
                _mm256_loadu_ps(
                    yTopLine +
                    outputX);

            const __m256 yBottom =
                _mm256_loadu_ps(
                    yBottomLine +
                    outputX);

            const __m256 yNext =
                _mm256_loadu_ps(
                    yNextLine +
                    outputX);

            const __m256 term0 =
                _mm256_mul_ps(
                    two,
                    yTop);

            const __m256 term1 =
                _mm256_mul_ps(
                    _mm256_sub_ps(
                        yBottom,
                        yPrevious),
                    tVector);

            const __m256 term2 =
                _mm256_mul_ps(
                    _mm256_sub_ps(
                        _mm256_add_ps(
                            _mm256_mul_ps(
                                two,
                                yPrevious),
                            _mm256_mul_ps(
                                four,
                                yBottom)),
                        _mm256_add_ps(
                            _mm256_mul_ps(
                                five,
                                yTop),
                            yNext)),
                    t2Vector);

            const __m256 term3 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_sub_ps(
                            yNext,
                            yPrevious),
                        _mm256_mul_ps(
                            three,
                            _mm256_sub_ps(
                                yTop,
                                yBottom))),
                    t3Vector);

            const __m256 interpolatedY =
                _mm256_mul_ps(
                    half,
                    _mm256_add_ps(
                        _mm256_add_ps(
                            term0,
                            term1),
                        _mm256_add_ps(
                            term2,
                            term3)));

            const __m256 uTop =
                _mm256_loadu_ps(
                    uTopLine +
                    outputX);

            const __m256 uBottom =
                _mm256_loadu_ps(
                    uBottomLine +
                    outputX);

            const __m256 vTop =
                _mm256_loadu_ps(
                    vTopLine +
                    outputX);

            const __m256 vBottom =
                _mm256_loadu_ps(
                    vBottomLine +
                    outputX);

            const __m256 interpolatedU =
                _mm256_add_ps(
                    uTop,
                    _mm256_mul_ps(
                        _mm256_sub_ps(
                            uBottom,
                            uTop),
                        chromaVerticalFraction));

            const __m256 interpolatedV =
                _mm256_add_ps(
                    vTop,
                    _mm256_mul_ps(
                        _mm256_sub_ps(
                            vBottom,
                            vTop),
                        chromaVerticalFraction));

            const __m256i yVector =
                _mm256_sub_epi32(
                    _mm256_cvttps_epi32(
                        _mm256_mul_ps(
                            interpolatedY,
                            scaleTo8Bit)),
                    yOffset);

            const __m256i uVector =
                _mm256_sub_epi32(
                    _mm256_cvttps_epi32(
                        _mm256_mul_ps(
                            interpolatedU,
                            scaleTo8Bit)),
                    chromaOffset);

            const __m256i vVector =
                _mm256_sub_epi32(
                    _mm256_cvttps_epi32(
                        _mm256_mul_ps(
                            interpolatedV,
                            scaleTo8Bit)),
                    chromaOffset);

            const __m256i c =
                _mm256_mullo_epi32(
                    yVector,
                    coefficientY);

            __m256i red =
                _mm256_add_epi32(
                    c,
                    _mm256_mullo_epi32(
                        vVector,
                        coefficientRV));

            red =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        red,
                        rounding),
                    8);

            __m256i green =
                _mm256_sub_epi32(
                    c,
                    _mm256_mullo_epi32(
                        uVector,
                        coefficientGU));

            green =
                _mm256_sub_epi32(
                    green,
                    _mm256_mullo_epi32(
                        vVector,
                        coefficientGV));

            green =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        green,
                        rounding),
                    8);

            __m256i blue =
                _mm256_add_epi32(
                    c,
                    _mm256_mullo_epi32(
                        uVector,
                        coefficientBU));

            blue =
                _mm256_srai_epi32(
                    _mm256_add_epi32(
                        blue,
                        rounding),
                    8);

            red =
                clampInt32ToByteRange(
                    red);

            green =
                clampInt32ToByteRange(
                    green);

            blue =
                clampInt32ToByteRange(
                    blue);

            __m256i outR =
                _mm256_i32gather_epi32(
                    displayLut32_.data(),
                    red,
                    4);

            __m256i outG =
                _mm256_i32gather_epi32(
                    displayLut32_.data(),
                    green,
                    4);

            __m256i outB =
                _mm256_i32gather_epi32(
                    displayLut32_.data(),
                    blue,
                    4);

            if (highlightThisLine)
            {
                const __m256i xPositions =
                    _mm256_setr_epi32(
                        outputX + 0,
                        outputX + 1,
                        outputX + 2,
                        outputX + 3,
                        outputX + 4,
                        outputX + 5,
                        outputX + 6,
                        outputX + 7);

                const __m256i highlightStartVector =
                    _mm256_set1_epi32(
                        highlightedOutputStartX);

                const __m256i highlightEndVector =
                    _mm256_set1_epi32(
                        highlightedOutputEndX);

                const __m256i atOrAfterStart =
                    _mm256_cmpgt_epi32(
                        xPositions,
                        _mm256_sub_epi32(
                            highlightStartVector,
                            _mm256_set1_epi32(1)));

                const __m256i beforeEnd =
                    _mm256_cmpgt_epi32(
                        highlightEndVector,
                        xPositions);

                const __m256i highlightMask =
                    _mm256_and_si256(
                        atOrAfterStart,
                        beforeEnd);

                outR =
                    _mm256_blendv_epi8(
                        outR,
                        _mm256_sub_epi32(
                            maximum,
                            outR),
                        highlightMask);

                outG =
                    _mm256_blendv_epi8(
                        outG,
                        _mm256_sub_epi32(
                            maximum,
                            outG),
                        highlightMask);

                outB =
                    _mm256_blendv_epi8(
                        outB,
                        _mm256_sub_epi32(
                            maximum,
                            outB),
                        highlightMask);
            }

            __m256i pixels =
                alpha;

            pixels =
                _mm256_or_si256(
                    pixels,
                    _mm256_slli_epi32(
                        outR,
                        16));

            pixels =
                _mm256_or_si256(
                    pixels,
                    _mm256_slli_epi32(
                        outG,
                        8));

            pixels =
                _mm256_or_si256(
                    pixels,
                    outB);

            _mm256_storeu_si256(
                reinterpret_cast<__m256i*>(
                    dst + outputX),
                pixels);
        }

        for (;
            outputX < outputWidth;
            ++outputX)
        {
            const float yPrevious =
                yPreviousLine[outputX];

            const float yTop =
                yTopLine[outputX];

            const float yBottom =
                yBottomLine[outputX];

            const float yNext =
                yNextLine[outputX];

            const float interpolatedY =
                0.5f *
                (
                    2.0f * yTop +
                    (-yPrevious + yBottom) * t +
                    (2.0f * yPrevious -
                        5.0f * yTop +
                        4.0f * yBottom -
                        yNext) * t2 +
                    (-yPrevious +
                        3.0f * yTop -
                        3.0f * yBottom +
                        yNext) * t3
                    );

            const float interpolatedU =
                uTopLine[outputX] +
                (uBottomLine[outputX] -
                    uTopLine[outputX]) *
                verticalFraction;

            const float interpolatedV =
                vTopLine[outputX] +
                (vBottomLine[outputX] -
                    vTopLine[outputX]) *
                verticalFraction;

            const int yy =
                static_cast<int>(
                    interpolatedY /
                    256.0f) -
                16;

            const int u =
                static_cast<int>(
                    interpolatedU /
                    256.0f) -
                128;

            const int v =
                static_cast<int>(
                    interpolatedV /
                    256.0f) -
                128;

            const int c =
                298 * yy;

            const int r =
                (c +
                    409 * v +
                    128) >>
                8;

            const int g =
                (c -
                    100 * u -
                    208 * v +
                    128) >>
                8;

            const int b =
                (c +
                    516 * u +
                    128) >>
                8;

            int outR =
                qBound(
                    0,
                    r,
                    255);

            int outG =
                qBound(
                    0,
                    g,
                    255);

            int outB =
                qBound(
                    0,
                    b,
                    255);

            outR =
                displayLut_[
                    static_cast<std::size_t>(
                        outR)];

            outG =
                displayLut_[
                    static_cast<std::size_t>(
                        outG)];

            outB =
                displayLut_[
                    static_cast<std::size_t>(
                        outB)];

            if (highlightThisLine &&
                outputX >= highlightedOutputStartX &&
                outputX < highlightedOutputEndX)
            {
                outR =
                    255 -
                    outR;

                outG =
                    255 -
                    outG;

                outB =
                    255 -
                    outB;
            }

            dst[outputX] =
                qRgb(
                    outR,
                    outG,
                    outB);
        }
    }

    performance.composeUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    return image;
}

void DisplayConverter::setHighlightedRange(
    int startX,
    int endX)
{
    highlightedStartX_ =
        startX;

    highlightedEndX_ =
        endX;
}