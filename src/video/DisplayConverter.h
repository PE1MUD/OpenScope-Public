#pragma once

#include "Yuv444Frame.h"
#include "video/DisplayPerformance.h"

#include <QImage>

#include <vector>
#include <array>
#include <cstdint>

enum class DisplayConversionImplementation
{
    Scalar,
    Avx2
};

class DisplayConverter
{
public:
    QImage convert(
        const Yuv444Frame& frame,
        const std::uint16_t* luma,
        int outputWidth,
        int outputHeight,
        DisplayPerformance& performance) const;

    void convertRange(
        const Yuv444Frame& frame,
        const std::uint16_t* luma,
        QRgb* outputPixels,
        int outputStridePixels,
        int outputWidth,
        int outputHeight,
        int firstOutputY,
        int lastOutputY,
        DisplayPerformance& performance) const;

    // Fast native-size path used by the display workers for one Spout
    // field. The destination image is allocated by the coordinator; workers
    // own non-overlapping scanline ranges.
    bool convertNativeRange(
        const Yuv444Frame& frame,
        const std::uint16_t* luma,
        QRgb* outputPixels,
        int outputStridePixels,
        int firstOutputY,
        int lastOutputY,
        DisplayPerformance& performance) const;

    // Fast path for the fixed-size 50 fps video Spout output.
    // Both deinterlaced luma fields share the same U/V planes, so
    // convert the pair in one native-size AVX2 pass and reuse the
    // chroma work for both output images.
    bool convertNativePair(
        const Yuv444Frame& frame,
        const std::uint16_t* firstLuma,
        const std::uint16_t* secondLuma,
        QImage& firstImage,
        QImage& secondImage,
        DisplayPerformance& performance) const;

    void setHighlightedLine(int line);
    DisplayConverter();
    void setGamma(double gamma);
    void setImplementation(
        DisplayConversionImplementation implementation);

    void setHighlightedRange(
        int startX,
        int endX);

private:
    void rebuildDisplayLut();
    std::array<std::uint8_t, 256> displayLut_;
    std::array<int, 256> yToC_;
    std::array<int, 256> vToRed_;
    std::array<int, 256> uToGreen_;
    std::array<int, 256> vToGreen_;
    std::array<int, 256> uToBlue_;
    std::array<int, 256> displayLut32_;

    mutable std::vector<int> horizontalLeftIndex_;
    mutable std::vector<int> horizontalRightIndex_;
    mutable std::vector<float> horizontalFraction_;

    mutable std::array<std::vector<float>, 4> resampledYLines_;
    mutable std::array<std::vector<float>, 2> resampledULines_;
    mutable std::array<std::vector<float>, 2> resampledVLines_;

    mutable int cachedInputWidth_ = 0;
    mutable int cachedOutputWidth_ = 0;
    double displayGamma_ = 1.0;

    int highlightedLine_ = -1;
    void rebuildColorConversionLuts();
    DisplayConversionImplementation implementation_{
        DisplayConversionImplementation::Scalar
    };
    QImage convertScalar(
        const Yuv444Frame& frame,
        const std::uint16_t* luma,
        QRgb* outputPixels,
        int outputStridePixels,
        int outputWidth,
        int outputHeight,
        int firstOutputY,
        int lastOutputY,
        DisplayPerformance& performance) const;

    QImage convertAvx2(
        const Yuv444Frame& frame,
        const std::uint16_t* luma,
        QRgb* outputPixels,
        int outputStridePixels,
        int outputWidth,
        int outputHeight,
        int firstOutputY,
        int lastOutputY,
        DisplayPerformance& performance) const;

    int highlightedStartX_ = 0;
    int highlightedEndX_ = -1;
};