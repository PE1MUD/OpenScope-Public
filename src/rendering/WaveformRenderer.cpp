#include "rendering/WaveformRenderer.h"
#include "diagnostics/TraceLog.h"
#include "ui/ViewportOverlay.h"
#include "processing/SignalReconstructor.h"
#include "standards/VideoStandard.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QColor>
#include <QPen>
#include <QPointF>
#include <QtGlobal>
#include <QApplication>

// CatWuzle keeps the accepted 0.8.0 visual baseline. The core raster may
// be split into deterministic, disjoint target-X chunks; geometry and pixel
// coverage remain identical to the scalar path.

#include <algorithm>
#include <numeric>
#include <bit>
#include <array>
#include <cmath>
#include <numbers>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <immintrin.h>

namespace
{



    struct GlowWorkload
    {
        std::uint32_t dirtyTiles = 0;
        std::uint32_t totalTiles = 0;
        std::uint32_t horizontalPass1Tiles = 0;
        std::uint32_t verticalPass1Tiles = 0;
        std::uint32_t horizontalPass2Tiles = 0;
        std::uint32_t verticalPass2Tiles = 0;
        int activeX = 0;
        int activeY = 0;
        int activeWidth = 0;
        int activeHeight = 0;
    };

    GlowWorkload applyHalfResolutionWhiteGlow(
        QImage& image,
        int glow)
    {
        GlowWorkload workload;
        if (glow <= 0 || image.isNull() ||
            image.width() < 2 || image.height() < 2)
        {
            return workload;
        }

        const int width = image.width();
        const int height = image.height();
        const int lowWidth = (width + 1) / 2;
        const int lowHeight = (height + 1) / 2;
        const std::size_t lowCount =
            static_cast<std::size_t>(lowWidth) *
            static_cast<std::size_t>(lowHeight);

        // Diagnostic tiling only.  These are glow-buffer tiles, not render
        // jobs yet.  Keeping the accounting in the already-required source
        // extraction pass avoids an extra full-image scan.
        constexpr int kTileWidth = 64;
        constexpr int kTileHeight = 32;
        const int tilesX = (lowWidth + kTileWidth - 1) / kTileWidth;
        const int tilesY = (lowHeight + kTileHeight - 1) / kTileHeight;
        workload.totalTiles = static_cast<std::uint32_t>(tilesX * tilesY);

        static thread_local std::vector<std::uint8_t> dirtyTiles;
        dirtyTiles.assign(
            static_cast<std::size_t>(workload.totalTiles),
            static_cast<std::uint8_t>(0));

        static thread_local std::vector<std::uint32_t> sourceActivity;
        static thread_local std::vector<std::uint32_t> horizontal1Activity;
        static thread_local std::vector<std::uint32_t> vertical1Activity;
        static thread_local std::vector<std::uint32_t> horizontal2Activity;
        static thread_local std::vector<std::uint32_t> vertical2Activity;
        sourceActivity.assign(dirtyTiles.size(), 0u);

        int minActiveX = lowWidth;
        int minActiveY = lowHeight;
        int maxActiveX = -1;
        int maxActiveY = -1;

        static thread_local std::vector<std::uint16_t> source;
        static thread_local std::vector<std::uint16_t> temp;
        static thread_local std::vector<std::uint16_t> blur;

        source.resize(lowCount);
        temp.resize(lowCount);
        blur.resize(lowCount);

        // Extract only the white component of the rendered trace. This keeps
        // the luma beam glowing without turning the coloured chroma fill into
        // a broad white haze.
        for (int ly = 0; ly < lowHeight; ++ly)
        {
            const int y0 = ly * 2;
            const int y1 = std::min(y0 + 1, height - 1);

            const auto* line0 = reinterpret_cast<const QRgb*>(image.constScanLine(y0));
            const auto* line1 = reinterpret_cast<const QRgb*>(image.constScanLine(y1));

            for (int lx = 0; lx < lowWidth; ++lx)
            {
                const int x0 = lx * 2;
                const int x1 = std::min(x0 + 1, width - 1);

                const auto white = [](QRgb pixel)
                {
                    return std::min({ qRed(pixel), qGreen(pixel), qBlue(pixel) });
                };

                const int value = std::max({
                    white(line0[x0]),
                    white(line0[x1]),
                    white(line1[x0]),
                    white(line1[x1])
                });

                source[static_cast<std::size_t>(ly) * lowWidth + lx] =
                    static_cast<std::uint16_t>(value);

                if (value > 0)
                {
                    minActiveX = std::min(minActiveX, lx);
                    minActiveY = std::min(minActiveY, ly);
                    maxActiveX = std::max(maxActiveX, lx);
                    maxActiveY = std::max(maxActiveY, ly);

                    const int tileX = lx / kTileWidth;
                    const int tileY = ly / kTileHeight;
                    const std::size_t tileIndex =
                        static_cast<std::size_t>(tileY) *
                        static_cast<std::size_t>(tilesX) +
                        static_cast<std::size_t>(tileX);

                    ++sourceActivity[tileIndex];

                    if (dirtyTiles[tileIndex] == 0)
                    {
                        dirtyTiles[tileIndex] = 1;
                        ++workload.dirtyTiles;
                    }
                }
            }
        }

        if (maxActiveX >= minActiveX && maxActiveY >= minActiveY)
        {
            workload.activeX = minActiveX * 2;
            workload.activeY = minActiveY * 2;
            workload.activeWidth =
                std::min(width - workload.activeX,
                    (maxActiveX - minActiveX + 1) * 2);
            workload.activeHeight =
                std::min(height - workload.activeY,
                    (maxActiveY - minActiveY + 1) * 2);
        }

        // Two cheap box passes approximate a Gaussian. The blur is done at
        // half resolution. From this point on, only active tiles are
        // processed. A tile propagates into neighbouring tiles only while
        // at least 5% of that tile still contains non-zero glow data.
        const double renderScale =
            static_cast<double>(height) / 576.0;
        const int radius = std::max(1, static_cast<int>(std::lround(renderScale)));
        const int horizontalTileExpansion =
            (radius + kTileWidth - 1) / kTileWidth;
        const int verticalTileExpansion =
            (radius + kTileHeight - 1) / kTileHeight;

        static thread_local std::vector<std::uint8_t> horizontalMask1;
        static thread_local std::vector<std::uint8_t> verticalMask1;
        static thread_local std::vector<std::uint8_t> horizontalMask2;
        static thread_local std::vector<std::uint8_t> verticalMask2;

        constexpr std::uint32_t kMinimumExpansionPercent = 5;

        const auto dilateTiles =
            [lowWidth, lowHeight, tilesX, tilesY, kTileWidth, kTileHeight](
                const std::vector<std::uint8_t>& input,
                const std::vector<std::uint32_t>& activity,
                std::vector<std::uint8_t>& output,
                int expandX,
                int expandY)
            {
                // The tile itself remains active. It only propagates glow to
                // neighbouring tiles while at least 5% of that tile still
                // contains non-zero glow data from the preceding pass.
                output = input;

                for (int tileY = 0; tileY < tilesY; ++tileY)
                {
                    const int pixelFirstY = tileY * kTileHeight;
                    const int pixelLastY =
                        std::min(pixelFirstY + kTileHeight, lowHeight);

                    for (int tileX = 0; tileX < tilesX; ++tileX)
                    {
                        const std::size_t index =
                            static_cast<std::size_t>(tileY) *
                            static_cast<std::size_t>(tilesX) +
                            static_cast<std::size_t>(tileX);

                        if (input[index] == 0)
                        {
                            continue;
                        }

                        const int pixelFirstX = tileX * kTileWidth;
                        const int pixelLastX =
                            std::min(pixelFirstX + kTileWidth, lowWidth);
                        const std::uint32_t tilePixels =
                            static_cast<std::uint32_t>(
                                (pixelLastX - pixelFirstX) *
                                (pixelLastY - pixelFirstY));

                        if (activity[index] * 100u <
                            tilePixels * kMinimumExpansionPercent)
                        {
                            continue;
                        }

                        const int firstY = std::max(0, tileY - expandY);
                        const int lastY = std::min(tilesY - 1, tileY + expandY);
                        const int firstX = std::max(0, tileX - expandX);
                        const int lastX = std::min(tilesX - 1, tileX + expandX);

                        for (int y = firstY; y <= lastY; ++y)
                        {
                            for (int x = firstX; x <= lastX; ++x)
                            {
                                output[
                                    static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(tilesX) +
                                    static_cast<std::size_t>(x)] = 1;
                            }
                        }
                    }
                }
            };

        const auto countActiveTiles =
            [](const std::vector<std::uint8_t>& mask)
            {
                return static_cast<std::uint32_t>(
                    std::count_if(
                        mask.begin(),
                        mask.end(),
                        [](std::uint8_t value)
                        {
                            return value != 0;
                        }));
            };

        const auto blurHorizontal =
            [lowWidth, lowHeight, radius, tilesX, kTileWidth, kTileHeight](
                const std::vector<std::uint16_t>& in,
                std::vector<std::uint16_t>& out,
                const std::vector<std::uint8_t>& mask,
                std::vector<std::uint32_t>& activity)
            {
                std::fill(out.begin(), out.end(), static_cast<std::uint16_t>(0));
                activity.assign(mask.size(), 0u);
                const std::uint32_t count =
                    static_cast<std::uint32_t>(2 * radius + 1);

                for (int tileY = 0; tileY < (lowHeight + kTileHeight - 1) / kTileHeight; ++tileY)
                {
                    const int firstY = tileY * kTileHeight;
                    const int lastY = std::min(firstY + kTileHeight, lowHeight);

                    for (int tileX = 0; tileX < tilesX; ++tileX)
                    {
                        const std::size_t tileIndex =
                            static_cast<std::size_t>(tileY) *
                            static_cast<std::size_t>(tilesX) +
                            static_cast<std::size_t>(tileX);

                        if (mask[tileIndex] == 0)
                        {
                            continue;
                        }

                        const int firstX = tileX * kTileWidth;
                        const int lastX = std::min(firstX + kTileWidth, lowWidth);

                        for (int y = firstY; y < lastY; ++y)
                        {
                            const std::size_t row =
                                static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(lowWidth);
                            std::uint32_t sum = 0;

                            for (int x = firstX - radius;
                                x <= firstX + radius;
                                ++x)
                            {
                                const int sx = std::clamp(x, 0, lowWidth - 1);
                                sum += in[row + static_cast<std::size_t>(sx)];
                            }

                            for (int x = firstX; x < lastX; ++x)
                            {
                                const std::uint16_t value =
                                    static_cast<std::uint16_t>(sum / count);
                                out[row + static_cast<std::size_t>(x)] = value;
                                if (value != 0)
                                {
                                    ++activity[tileIndex];
                                }

                                const int removeX =
                                    std::clamp(x - radius, 0, lowWidth - 1);
                                const int addX =
                                    std::clamp(x + radius + 1, 0, lowWidth - 1);
                                sum -= in[row + static_cast<std::size_t>(removeX)];
                                sum += in[row + static_cast<std::size_t>(addX)];
                            }
                        }
                    }
                }
            };

        const auto blurVertical =
            [lowWidth, lowHeight, radius, tilesX, kTileWidth, kTileHeight](
                const std::vector<std::uint16_t>& in,
                std::vector<std::uint16_t>& out,
                const std::vector<std::uint8_t>& mask,
                std::vector<std::uint32_t>& activity)
            {
                std::fill(out.begin(), out.end(), static_cast<std::uint16_t>(0));
                activity.assign(mask.size(), 0u);
                const std::uint32_t count =
                    static_cast<std::uint32_t>(2 * radius + 1);
                const int tilesY =
                    (lowHeight + kTileHeight - 1) / kTileHeight;

                for (int tileY = 0; tileY < tilesY; ++tileY)
                {
                    const int firstY = tileY * kTileHeight;
                    const int lastY = std::min(firstY + kTileHeight, lowHeight);

                    for (int tileX = 0; tileX < tilesX; ++tileX)
                    {
                        const std::size_t tileIndex =
                            static_cast<std::size_t>(tileY) *
                            static_cast<std::size_t>(tilesX) +
                            static_cast<std::size_t>(tileX);

                        if (mask[tileIndex] == 0)
                        {
                            continue;
                        }

                        const int firstX = tileX * kTileWidth;
                        const int lastX = std::min(firstX + kTileWidth, lowWidth);

                        for (int x = firstX; x < lastX; ++x)
                        {
                            std::uint32_t sum = 0;

                            for (int y = firstY - radius;
                                y <= firstY + radius;
                                ++y)
                            {
                                const int sy = std::clamp(y, 0, lowHeight - 1);
                                sum += in[
                                    static_cast<std::size_t>(sy) *
                                    static_cast<std::size_t>(lowWidth) +
                                    static_cast<std::size_t>(x)];
                            }

                            for (int y = firstY; y < lastY; ++y)
                            {
                                const std::size_t index =
                                    static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(lowWidth) +
                                    static_cast<std::size_t>(x);
                                const std::uint16_t value =
                                    static_cast<std::uint16_t>(sum / count);
                                out[index] = value;
                                if (value != 0)
                                {
                                    ++activity[tileIndex];
                                }

                                const int removeY =
                                    std::clamp(y - radius, 0, lowHeight - 1);
                                const int addY =
                                    std::clamp(y + radius + 1, 0, lowHeight - 1);
                                sum -= in[
                                    static_cast<std::size_t>(removeY) *
                                    static_cast<std::size_t>(lowWidth) +
                                    static_cast<std::size_t>(x)];
                                sum += in[
                                    static_cast<std::size_t>(addY) *
                                    static_cast<std::size_t>(lowWidth) +
                                    static_cast<std::size_t>(x)];
                            }
                        }
                    }
                }
            };

        dilateTiles(
            dirtyTiles,
            sourceActivity,
            horizontalMask1,
            horizontalTileExpansion,
            0);
        workload.horizontalPass1Tiles = countActiveTiles(horizontalMask1);
        blurHorizontal(source, temp, horizontalMask1, horizontal1Activity);

        dilateTiles(
            horizontalMask1,
            horizontal1Activity,
            verticalMask1,
            0,
            verticalTileExpansion);
        workload.verticalPass1Tiles = countActiveTiles(verticalMask1);
        blurVertical(temp, blur, verticalMask1, vertical1Activity);

        dilateTiles(
            verticalMask1,
            vertical1Activity,
            horizontalMask2,
            horizontalTileExpansion,
            0);
        workload.horizontalPass2Tiles = countActiveTiles(horizontalMask2);
        blurHorizontal(blur, temp, horizontalMask2, horizontal2Activity);

        dilateTiles(
            horizontalMask2,
            horizontal2Activity,
            verticalMask2,
            0,
            verticalTileExpansion);
        workload.verticalPass2Tiles = countActiveTiles(verticalMask2);
        blurVertical(temp, blur, verticalMask2, vertical2Activity);

        const int glowScale = std::clamp(glow, 0, 100);

        // Composite only the tiles kept by the final thresholded blur mask.
        for (int tileY = 0; tileY < tilesY; ++tileY)
        {
            for (int tileX = 0; tileX < tilesX; ++tileX)
            {
                const std::size_t tileIndex =
                    static_cast<std::size_t>(tileY) *
                    static_cast<std::size_t>(tilesX) +
                    static_cast<std::size_t>(tileX);

                if (verticalMask2[tileIndex] == 0)
                {
                    continue;
                }

                const int firstX = tileX * kTileWidth * 2;
                const int lastX =
                    std::min((tileX + 1) * kTileWidth * 2, width);
                const int firstY = tileY * kTileHeight * 2;
                const int lastY =
                    std::min((tileY + 1) * kTileHeight * 2, height);

                for (int y = firstY; y < lastY; ++y)
                {
                    auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
                    const int ly = std::min(y / 2, lowHeight - 1);

                    for (int x = firstX; x < lastX; ++x)
                    {
                        const int lx = std::min(x / 2, lowWidth - 1);
                        const int blurred =
                            blur[static_cast<std::size_t>(ly) * lowWidth + lx];
                        const int contribution =
                            (blurred * glowScale * 3) / 400;

                        if (contribution <= 0)
                        {
                            continue;
                        }

                        const QRgb pixel = line[x];
                        line[x] = qRgb(
                            std::min(255, qRed(pixel) + contribution),
                            std::min(255, qGreen(pixel) + contribution),
                            std::min(255, qBlue(pixel) + contribution));
                    }
                }
            }
        }

        return workload;
    }

    constexpr double kMaximumSampleValue = 65535.0;
    constexpr double kChromaNoiseThresholdFraction =
        0.03;
    constexpr int kLuminanceBeamIntensity = 256;
    constexpr int kChromaBeamIntensity = 768;
    constexpr std::uint32_t kConnectorIntensity = 160u;

    // Keep accumulated chroma energy below full scale.
    // This prevents high Scopephor settings from driving
    // the RGB chroma channels into long-term saturation.
    constexpr std::uint32_t kMaximumChromaLevel = 16384u;
}

void WaveformRenderer::setChromaFillIntensity(
    int intensity)
{
    chromaFillIntensity_ =
        std::clamp(
            intensity,
            0,
            200);
}

void WaveformRenderer::setColor(bool enabled)
{
    settings_.color = enabled;
}

void WaveformRenderer::setMeasurementProbePresentation(
    bool enabled,
    double normalizedX,
    double volts)
{
    measurementProbeNormalizedX_.store(
        std::clamp(normalizedX, 0.0, 1.0),
        std::memory_order_relaxed);

    measurementProbeVolts_.store(
        volts,
        std::memory_order_relaxed);

    measurementProbePresentation_.store(
        enabled,
        std::memory_order_relaxed);
}

void WaveformRenderer::setTraceJobExecutor(
    TraceJobExecutor executor)
{
    traceJobExecutor_ = std::move(executor);
}

void WaveformRenderer::setTraceHelperAvailability(
    TraceHelperAvailability availability)
{
    traceHelperAvailability_ = std::move(availability);
}

void WaveformRenderer::setLineInfoOverlayEnabled(
    bool enabled,
    bool palOutput)
{
    lineInfoOverlayEnabled_ = enabled;
    lineInfoOverlayPalOutput_ = palOutput;
}

void addFillPixel(
    QImage& image,
    int x,
    int y,
    int red,
    int green,
    int blue,
    int intensity)
{
    if (x < 0 ||
        x >= image.width() ||
        y < 0 ||
        y >= image.height())
    {
        return;
    }

    QRgb* scanLine =
        reinterpret_cast<QRgb*>(
            image.scanLine(y));

    const QRgb current =
        scanLine[x];

    const int newRed =
        std::min(
            255,
            qRed(current) +
            red * intensity / 255);

    const int newGreen =
        std::min(
            255,
            qGreen(current) +
            green * intensity / 255);

    const int newBlue =
        std::min(
            255,
            qBlue(current) +
            blue * intensity / 255);

    scanLine[x] =
        qRgb(
            newRed,
            newGreen,
            newBlue);
}

void resampleLinear(
    std::span<const float> input,
    std::span<float> output)
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

    const float scale =
        static_cast<float>(input.size()) /
        static_cast<float>(output.size());

    for (std::size_t outputIndex = 0;
        outputIndex < output.size();
        ++outputIndex)
    {
        const float sourcePosition =
            (static_cast<float>(outputIndex) + 0.5f) *
            scale -
            0.5f;

        const int leftIndex =
            std::clamp(
                static_cast<int>(
                    std::floor(sourcePosition)),
                0,
                static_cast<int>(
                    input.size()) - 1);

        const int rightIndex =
            std::min(
                leftIndex + 1,
                static_cast<int>(
                    input.size()) - 1);

        const float fraction =
            std::clamp(
                sourcePosition -
                static_cast<float>(leftIndex),
                0.0f,
                1.0f);

        const float left =
            input[
                static_cast<std::size_t>(
                    leftIndex)];

        const float right =
            input[
                static_cast<std::size_t>(
                    rightIndex)];

        output[outputIndex] =
            left +
            (right - left) *
            fraction;
    }
}
void resampleMinMax(
    std::span<const float> input,
    std::span<float> outputMin,
    std::span<float> outputMax)
{
    if (input.empty() ||
        outputMin.empty() ||
        outputMax.empty())
    {
        return;
    }

    const std::size_t outputSize =
        std::min(
            outputMin.size(),
            outputMax.size());

    for (std::size_t x = 0;
        x < outputSize;
        ++x)
    {
        const std::size_t first =
            x * input.size() /
            outputSize;

        const std::size_t last =
            std::max(
                first + 1u,
                (x + 1u) * input.size() /
                outputSize);

        float minimum = input[first];
        float maximum = input[first];

        for (std::size_t sourceIndex = first + 1u;
            sourceIndex < last &&
            sourceIndex < input.size();
            ++sourceIndex)
        {
            minimum =
                std::min(
                    minimum,
                    input[sourceIndex]);

            maximum =
                std::max(
                    maximum,
                    input[sourceIndex]);
        }

        outputMin[x] = minimum;
        outputMax[x] = maximum;
    }
}
WaveformRenderer::WaveformRenderer()
    : image_(
        VideoStandard::pal625().outputWidth,
        VideoStandard::pal625().outputHeight,
        QImage::Format_RGB32)
    , hits_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth) *
        static_cast<std::size_t>(
            VideoStandard::pal625().outputHeight),
        0u)
    , allLinesPersistence_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth) *
        static_cast<std::size_t>(
            VideoStandard::pal625().outputHeight),
        0.0f)
    , trace_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth) *
        static_cast<std::size_t>(
            VideoStandard::pal625().outputHeight))
    , chromaTrace_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth) *
        static_cast<std::size_t>(
            VideoStandard::pal625().outputHeight))
    , displayY_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth))
    , displayYMin_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth))
    , displayYMax_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth))
    , displayU_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth))
    , displayV_(
        static_cast<std::size_t>(
            VideoStandard::pal625().outputWidth))
{
    for (std::size_t i = 0;
        i < displayLut_.size();
        ++i)
    {
        displayLut_[i] =
            static_cast<std::uint8_t>(
                std::min<std::size_t>(
                    i,
                    255u));
    }

    image_.fill(Qt::black);

    imageSpare1_ = QImage(
        image_.width(),
        image_.height(),
        QImage::Format_RGB32);

    imageSpare2_ = QImage(
        image_.width(),
        image_.height(),
        QImage::Format_RGB32);
}

void WaveformRenderer::setTraceRendererId(
    TraceRendererId rendererId) noexcept
{
    traceRendererId_ = rendererId;
}

void WaveformRenderer::setOutputSize(
    int width,
    int height)
{
    width =
        std::max(
            width,
            1);

    height =
        std::max(
            height,
            1);

    if (image_.width() == width &&
        image_.height() == height)
    {
        return;
    }

    const std::size_t requestedPixelCount =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    outputSizeChangedSinceRender_ = true;
    outputBufferCapacityGrewSinceRender_ =
        outputBufferCapacityGrewSinceRender_ ||
        trace_.capacity() < requestedPixelCount ||
        chromaTrace_.capacity() < requestedPixelCount ||
        hits_.capacity() < requestedPixelCount ||
        allLinesPersistence_.capacity() < requestedPixelCount;

    image_ =
        QImage(
            width,
            height,
            QImage::Format_RGB32);

    imageSpare1_ =
        QImage(
            width,
            height,
            QImage::Format_RGB32);

    imageSpare2_ =
        QImage(
            width,
            height,
            QImage::Format_RGB32);

    image_.fill(Qt::black);

    const std::size_t pixelCount =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    hits_.assign(
        pixelCount,
        0u);

    allLinesPersistence_.assign(
        pixelCount,
        0.0f);

    trace_.assign(
        pixelCount,
        TracePixel{});

    chromaTrace_.assign(
        pixelCount,
        TracePixel{});

    displayY_.resize(
        static_cast<std::size_t>(
            width));

    displayYMin_.resize(
        static_cast<std::size_t>(
            width));

    displayYMax_.resize(
        static_cast<std::size_t>(
            width));

    displayU_.resize(
        static_cast<std::size_t>(
            width));

    displayV_.resize(
        static_cast<std::size_t>(
            width));

    // Scopephor stores already-rendered images at the target resolution.
    // A target-size change therefore invalidates the complete phosphor stack.
    clearScopephorFrames();
}

void WaveformRenderer::setZoomed(
    bool zoomed)
{
    setZoomFactor(
        zoomed
        ? 10
        : 1);
}

void WaveformRenderer::setZoomFactor(
    int factor)
{
    if (factor != 1 &&
        factor != 5 &&
        factor != 10)
    {
        factor = 1;
    }

    if (zoomFactor_ == factor)
    {
        return;
    }

    qDebug()
        << "WaveformRenderer zoom factor:"
        << factor;

    zoomFactor_ = factor;
    clearTrace();
}


void WaveformRenderer::setContentScale(
    double scale)
{
    setContentScale(
        scale,
        scale);
}


void WaveformRenderer::setContentScale(
    double horizontalScale,
    double verticalScale)
{
    const double newHorizontalScale =
        std::clamp(
            horizontalScale,
            0.1,
            1.0);

    const double newVerticalScale =
        std::clamp(
            verticalScale,
            0.1,
            1.0);

    if (newHorizontalScale == contentScaleX_ &&
        newVerticalScale == contentScaleY_)
    {
        return;
    }

    contentScaleX_ = newHorizontalScale;
    contentScaleY_ = newVerticalScale;
    clearTrace();
}

void WaveformRenderer::setFitAspectRatio(bool enabled)
{
    fitAspectRatio_ = enabled;
}


void WaveformRenderer::setScrollPosition(
    double position)
{
    const double newPosition =
        std::clamp(
            position,
            0.0,
            1.0);

    if (newPosition == scrollPosition_)
    {
        return;
    }

    scrollPosition_ = newPosition;
}

void WaveformRenderer::prepareImageForRender()
{
    // QImage is implicitly shared. waveformChanged()/waveformVideoChanged() hand
    // the completed image to another thread; on the next frame a direct fill()
    // of that still-shared image would first detach and deep-copy the entire
    // previous framebuffer. At UHD-ish widget sizes that copy dominated B.
    //
    // Keep two same-resolution spare transport surfaces. In steady state one of
    // them is detached/free by the time the next frame needs a target, so the
    // renderer swaps to already-allocated memory instead of copying old pixels.
    // Only if the consumer is more than two frames behind do we fall back to a
    // fresh allocation; importantly that still avoids copying the old frame.
    if (image_.isDetached())
    {
        return;
    }

    auto trySwap =
        [this](QImage& spare)
        {
            if (spare.width() != image_.width() ||
                spare.height() != image_.height() ||
                spare.format() != image_.format() ||
                !spare.isDetached())
            {
                return false;
            }

            image_.swap(spare);
            return true;
        };

    if (trySwap(imageSpare1_) ||
        trySwap(imageSpare2_))
    {
        return;
    }

    // Exceptional back-pressure path: all three target surfaces are still
    // referenced by consumers. Do not detach/copy the old image; create a clean
    // replacement target. Normal steady-state rendering should never hit this.
    image_ = QImage(
        image_.width(),
        image_.height(),
        QImage::Format_RGB32);
}

void WaveformRenderer::analyze(
    const Yuv444Frame& frame)
{
    inputSampleClockHz_ =
        frame.sampleClockHz > 0.0
        ? frame.sampleClockHz
        : 13'500'000.0;

    inputSampleWidth_ =
        std::max(frame.width, 1);

    const std::size_t requiredSamples =
        frame.width > 0 &&
        frame.height > 0
        ? static_cast<std::size_t>(
            frame.width) *
        static_cast<std::size_t>(
            frame.height)
        : 0u;

    if (requiredSamples == 0u ||
        frame.y.size() < requiredSamples ||
        frame.u.size() < requiredSamples ||
        frame.v.size() < requiredSamples)
    {
        renderTimings_ = {};
        image_.fill(Qt::black);
        return;
    }

    if (selectedLine_ >= 0 &&
        selectedLine_ < frame.height)
    {
        renderSingleLine(
            frame);

        return;
    }

    renderAllLines(frame);
}


void WaveformRenderer::clearOrFadeTrace()
{
    if (persistence_ == 0)
    {
        std::fill(
            trace_.begin(),
            trace_.end(),
            TracePixel{});

        std::fill(
            chromaTrace_.begin(),
            chromaTrace_.end(),
            TracePixel{});

        return;
    }

    const std::uint32_t lumaPersistence =
        static_cast<std::uint32_t>(
            persistence_);

    constexpr std::uint32_t kMaximumChromaPersistence =
        204u;

    const std::uint32_t chromaPersistence =
        std::min(
            lumaPersistence,
            kMaximumChromaPersistence);

    const auto fadeTrace =
        [](std::vector<TracePixel>& pixels,
            std::uint32_t persistence)
        {
            for (TracePixel& pixel : pixels)
            {
                pixel.red =
                    static_cast<std::uint16_t>(
                        (static_cast<std::uint32_t>(
                            pixel.red) *
                            persistence) >>
                        8);

                pixel.green =
                    static_cast<std::uint16_t>(
                        (static_cast<std::uint32_t>(
                            pixel.green) *
                            persistence) >>
                        8);

                pixel.blue =
                    static_cast<std::uint16_t>(
                        (static_cast<std::uint32_t>(
                            pixel.blue) *
                            persistence) >>
                        8);
            }
        };

    fadeTrace(
        trace_,
        lumaPersistence);

    fadeTrace(
        chromaTrace_,
        chromaPersistence);
}

WaveformGraticuleLayout WaveformRenderer::graticuleLayout() const
{
    const QRectF canvasRect(
        0.0,
        0.0,
        static_cast<double>(
            std::max(1, image_.width() - 1)),
        static_cast<double>(
            std::max(1, image_.height() - 1)));

    return graticule_.layout(
        canvasRect,
        QApplication::font(),
        &image_,
        OpenScopeSettings::aspectRatioValue(
            aspectRatio_),
        fitAspectRatio_,
        contentScaleX_,
        contentScaleY_);
}

QRectF WaveformRenderer::viewportRect() const
{
    return graticuleLayout().viewportRect;
}

QRectF WaveformRenderer::scaledScopeRect() const
{
    return graticuleLayout().plotRect;
}

void WaveformRenderer::renderSingleLine(
    const Yuv444Frame& frame)
{
    renderTimings_ = {};

    // Quality-rebase reference path has no adaptive-AA activity overlay.

    QElapsedTimer frameTimer;
    frameTimer.start();

    const std::uint64_t logGeneration = ++traceLogGeneration_;
    traceLog(
        TraceEventType::WaveformBegin,
        logGeneration,
        0u,
        0u,
        static_cast<std::uint64_t>(image_.width()),
        static_cast<std::uint64_t>(image_.height()),
        traceRendererId_);

    const auto recordPhase =
        [this](char label,
            std::uint64_t startUs,
            std::uint64_t durationUs)
        {
            if (durationUs == 0u ||
                renderTimings_.phaseCount >=
                    WaveformRenderTimings::kPhaseCapacity)
            {
                return;
            }

            auto& event =
                renderTimings_.phases[
                    renderTimings_.phaseCount++];
            event.label = label;
            event.startUs = startUs;
            event.durationUs = durationUs;
        };

    renderTimings_.outputSizeChanged =
        outputSizeChangedSinceRender_;
    renderTimings_.outputBufferCapacityGrew =
        outputBufferCapacityGrewSinceRender_;
    renderTimings_.beamCoreRadiusPx =
        beamCoreRadiusPx_;
    renderTimings_.beamCoreMarginPx =
        std::max(
            1,
            static_cast<int>(
                std::floor(beamCoreRadiusPx_)));

    outputSizeChangedSinceRender_ = false;
    outputBufferCapacityGrewSinceRender_ = false;

    QElapsedTimer phaseTimer;
    phaseTimer.start();

    const std::size_t sourceWidth =
        static_cast<std::size_t>(
            frame.width);

    const std::size_t reconstructedWidth =
        sourceWidth * 4u;

    const std::size_t sourceLineOffset =
        static_cast<std::size_t>(
            selectedLine_) *
        sourceWidth;

    singleLineSource_.resize(sourceWidth);
    singleLineReconstructed_.resize(reconstructedWidth);

    const std::vector<float>* preparedLuma =
        preparedReconstructedLuma_;
    preparedReconstructedLuma_ = nullptr;

    const bool canReusePreparedLuma =
        preparedLuma != nullptr &&
        preparedLuma->size() == reconstructedWidth;

    if (canReusePreparedLuma)
    {
        std::copy(
            preparedLuma->begin(),
            preparedLuma->end(),
            singleLineReconstructed_.begin());
        renderTimings_.resamplerCacheRebuilt = false;
    }
    else
    {
        for (std::size_t x = 0;
            x < sourceWidth;
            ++x)
        {
            singleLineSource_[x] =
                static_cast<float>(
                    frame.y[sourceLineOffset + x]);
        }

        const std::uint64_t resamplerGenerationBefore =
            singleLineReconstructor_.cacheGeneration();

        const std::uint64_t upsampleStartUs =
            static_cast<std::uint64_t>(
                frameTimer.nsecsElapsed() / 1000);
        QElapsedTimer upsampleTimer;
        upsampleTimer.start();

        singleLineReconstructor_.resample(
            singleLineSource_,
            singleLineReconstructed_);

        const std::uint64_t upsampleUs =
            static_cast<std::uint64_t>(
                upsampleTimer.nsecsElapsed() / 1000);
        recordPhase(
            'U',
            upsampleStartUs,
            upsampleUs);

        renderTimings_.resamplerCacheRebuilt =
            singleLineReconstructor_.cacheGeneration() !=
            resamplerGenerationBefore;
    }

    fullLumaVolts_.resize(reconstructedWidth);

    const auto digitalLevels =
        levels(VideoStandard::pal625());

    const auto analog =
        analogLevels(
            VideoColorStandard::Rec601_625);

    const double voltsPerCode =
        (analog.whiteVolts - analog.blackVolts) /
        static_cast<double>(
            digitalLevels.yWhite -
            digitalLevels.yBlack);

    for (std::size_t x = 0;
        x < reconstructedWidth;
        ++x)
    {
        const double y10 =
            static_cast<double>(
                singleLineReconstructed_[x]) /
            64.0;

        fullLumaVolts_[x] =
            static_cast<float>(
                analog.blackVolts +
                (y10 - digitalLevels.yBlack) *
                voltsPerCode);
    }

    std::size_t reconstructedViewWidth =
        reconstructedWidth;
    std::size_t reconstructedViewOffset = 0u;

    if (zoomFactor_ > 1)
    {
        reconstructedViewWidth =
            std::max<std::size_t>(
                reconstructedWidth /
                    static_cast<std::size_t>(zoomFactor_),
                1u);

        const std::size_t maximumOffset =
            reconstructedWidth -
            reconstructedViewWidth;

        reconstructedViewOffset =
            static_cast<std::size_t>(
                scrollPosition_ *
                static_cast<double>(maximumOffset));
    }

    sourceY_.resize(reconstructedViewWidth);
    for (std::size_t x = 0;
        x < reconstructedViewWidth;
        ++x)
    {
        sourceY_[x] =
            fullLumaVolts_[
                reconstructedViewOffset + x];
    }

    // Chroma remains a current-frame envelope.  Scopephor history is the
    // reconstructed 2880-sample luminance beam only.
    std::size_t sourceViewWidth = sourceWidth;
    std::size_t sourceViewOffset = 0u;

    if (zoomFactor_ > 1)
    {
        sourceViewWidth =
            std::max<std::size_t>(
                sourceWidth /
                    static_cast<std::size_t>(zoomFactor_),
                1u);

        const std::size_t maximumSourceOffset =
            sourceWidth - sourceViewWidth;

        const double normalisedPosition =
            reconstructedWidth > reconstructedViewWidth
            ? static_cast<double>(reconstructedViewOffset) /
                static_cast<double>(
                    reconstructedWidth - reconstructedViewWidth)
            : 0.0;

        sourceViewOffset =
            static_cast<std::size_t>(
                normalisedPosition *
                static_cast<double>(maximumSourceOffset));
    }

    sourceU_.resize(sourceViewWidth);
    sourceV_.resize(sourceViewWidth);

    for (std::size_t x = 0;
        x < sourceViewWidth;
        ++x)
    {
        const std::size_t sourceX =
            std::min(
                sourceViewOffset + x,
                sourceWidth - 1u);

        const std::size_t sampleIndex =
            sourceLineOffset + sourceX;

        sourceU_[x] =
            static_cast<float>(frame.u[sampleIndex]);
        sourceV_[x] =
            static_cast<float>(frame.v[sampleIndex]);
    }

    displayY_.resize(
        static_cast<std::size_t>(image_.width()));
    displayU_.resize(
        static_cast<std::size_t>(image_.width()));
    displayV_.resize(
        static_cast<std::size_t>(image_.width()));

    resampleLinear(sourceY_, displayY_);
    resampleLinear(sourceU_, displayU_);
    resampleLinear(sourceV_, displayV_);

    const QRectF scope = scaledScopeRect();

    // Beam geometry must scale from the actual graticule plot area, not
    // from the outer transport canvas.  This keeps PC and PAL/Spout targets
    // physically consistent even when labels, aspect fitting or underscan
    // reduce the usable plot raster.
    const double plotRenderDimension =
        std::max(
            1.0,
            std::min(
                scope.width(),
                scope.height()));

    const double plotBeamScale =
        std::clamp(
            std::sqrt(
                plotRenderDimension / 576.0),
            1.0,
            1.90);

    constexpr double kReferenceCoreRadiusPx = 0.82;
    beamCoreRadiusPx_ =
        kReferenceCoreRadiusPx * plotBeamScale;

    renderTimings_.beamCoreRadiusPx =
        beamCoreRadiusPx_;
    renderTimings_.beamCoreMarginPx =
        std::max(
            1,
            static_cast<int>(
                std::floor(beamCoreRadiusPx_)));

    // -----------------------------------------------------------------
    // New Scopephor model:
    //
    //   current 2880-sample Y line
    //       -> one Catmull-Rom path in THIS target resolution
    //       -> render core + glow ONCE
    //       -> store the rendered target-resolution phosphor image
    //
    // Old phosphor memories are never splined or glowed again.
    // Display persistence is only a weighted sum of those stored images.
    // -----------------------------------------------------------------

    const std::uint64_t tracePrepStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();

    const std::vector<BeamPoint> currentLumaPolyline =
        buildCurrentLumaPolyline(
            scope,
            reconstructedViewOffset,
            reconstructedViewWidth);

    renderTimings_.tracePrepUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);
    recordPhase(
        'T',
        tracePrepStartUs,
        renderTimings_.tracePrepUs);

    // MUD sieve: classify the already-built target-space Catmull segments
    // before rasterisation.  TRUE means dense/steep packet: expensive AA may
    // be bypassed for that segment (except at the stitched packet edges).
    DenseSteepStats denseSteepStats;
    const std::uint64_t packetClassifyStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);
    QElapsedTimer mudTimer;
    mudTimer.start();

    const std::vector<bool> denseSteepSegment =
        buildDenseSteepPacketMask(
            currentLumaPolyline,
            &denseSteepStats);

    const std::uint64_t mudDetectUs =
        static_cast<std::uint64_t>(
            mudTimer.nsecsElapsed() / 1000);

    // This used to be the unexplained wallclock gap between T and the
    // first parallel raster chunk. Make it a first-class phase so the
    // composite Screen waveform timeline accounts for that time directly.
    recordPhase(
        'K',
        packetClassifyStartUs,
        mudDetectUs);

    traceLog(
        TraceEventType::MudDetect,
        logGeneration,
        0u,
        0u,
        mudDetectUs,
        denseSteepStats.neighbourProbes,
        traceRendererId_);
    traceLog(
        TraceEventType::MudDetect,
        logGeneration,
        0u,
        1u,
        denseSteepStats.runCount,
        denseSteepStats.sustainedRunCount,
        traceRendererId_);
    traceLog(
        TraceEventType::MudDetect,
        logGeneration,
        0u,
        2u,
        denseSteepStats.denseRunCount,
        denseSteepStats.acceptedPacketCount,
        traceRendererId_);
    traceLog(
        TraceEventType::MudDetect,
        logGeneration,
        0u,
        3u,
        denseSteepStats.whiteSegmentCount,
        denseSteepSegment.size(),
        traceRendererId_);

    std::uint64_t currentGlowUs = 0u;
    std::uint64_t currentCoreUs = 0u;

    int phosphorMinX = image_.width();
    int phosphorMinY = image_.height();
    int phosphorMaxX = -1;
    int phosphorMaxY = -1;

    CatWuzleFrameStats catWuzleFrameStats;

    const std::uint64_t traceRasterStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();

    renderCurrentPhosphorEnergy(
            currentLumaPolyline,
            denseSteepSegment,
            scope,
            currentGlowUs,
            currentCoreUs,
            phosphorMinX,
            phosphorMinY,
            phosphorMaxX,
            phosphorMaxY,
            catWuzleFrameStats,
            traceRasterStartUs);

    const std::uint64_t resolveSetupStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);

    auto& currentPhosphorEnergy = currentPhosphorEnergy_;

    renderTimings_.catWuzleChunkCount =
        catWuzleFrameStats.chunkCount;
    renderTimings_.catWuzleInvalidChunkCount =
        catWuzleFrameStats.invalidChunkCount;
    renderTimings_.catWuzleZipperUs =
        catWuzleFrameStats.zipperUs;
    renderTimings_.catWuzleChunkRenderMinUs =
        catWuzleFrameStats.chunkRenderMinUs;
    renderTimings_.catWuzleChunkRenderAvgUs =
        catWuzleFrameStats.chunkRenderAvgUs;
    renderTimings_.catWuzleChunkRenderMaxUs =
        catWuzleFrameStats.chunkRenderMaxUs;
    renderTimings_.catWuzleChunkQueueWaitMaxUs =
        catWuzleFrameStats.chunkQueueWaitMaxUs;
    renderTimings_.catWuzleWorkerChunkCount =
        catWuzleFrameStats.workerChunkCount;
    renderTimings_.catWuzleWorkerRenderUs =
        catWuzleFrameStats.workerRenderUs;

    const std::uint64_t traceRasterWallUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);
    const std::uint64_t zipperWallUs =
        std::min(
            catWuzleFrameStats.zipperUs,
            traceRasterWallUs);
    const std::uint64_t rasterBeforeZipperUs =
        traceRasterWallUs - zipperWallUs;

    recordPhase(
        'R',
        traceRasterStartUs,
        rasterBeforeZipperUs);
    recordPhase(
        'Z',
        traceRasterStartUs + rasterBeforeZipperUs,
        zipperWallUs);

    traceLog(
        TraceEventType::WaveformRaster,
        logGeneration,
        0u,
        0u,
        traceRasterWallUs,
        currentCoreUs,
        traceRendererId_);
    traceLog(
        TraceEventType::WaveformRaster,
        logGeneration,
        0u,
        1u,
        currentGlowUs,
        currentLumaPolyline.size(),
        traceRendererId_);

    QElapsedTimer resolveTimer;
    resolveTimer.start();

    /*
     * PRE-SCOPEPHOR AA RESOLVE
     * ------------------------
     *
     * The analytic 2x2 core is substantially cheaper than the 4x4 quality
     * experiment, but still leaves a small pixel-phase modulation which
     * Scopephor can otherwise preserve and make more visible.
     *
     * Resolve only the active beam bbox with a tiny separable 3-tap filter:
     *
     *     0.15   0.70   0.15
     *
     * Horizontal + vertical passes give a smooth subpixel filament before
     * temporal feedback, without touching the full framebuffer.  The kernel
     * sums to one, so beam energy is redistributed rather than amplified.
     */
    if (phosphorMaxX >= phosphorMinX &&
        phosphorMaxY >= phosphorMinY &&
        !currentPhosphorEnergy.empty())
    {
        const int resolveWidth = image_.width();
        const int resolveHeight = image_.height();

        const int resolveMinX =
            std::clamp(
                phosphorMinX - 1,
                0,
                resolveWidth - 1);

        const int resolveMaxX =
            std::clamp(
                phosphorMaxX + 1,
                0,
                resolveWidth - 1);

        const int resolveMinY =
            std::clamp(
                phosphorMinY - 1,
                0,
                resolveHeight - 1);

        const int resolveMaxY =
            std::clamp(
                phosphorMaxY + 1,
                0,
                resolveHeight - 1);

        const int resolveSpan =
            resolveMaxX -
            resolveMinX + 1;

        const int resolveRows =
            resolveMaxY -
            resolveMinY + 1;

        std::vector<std::uint16_t> resolveHorizontal(
            static_cast<std::size_t>(resolveSpan) *
                static_cast<std::size_t>(resolveRows),
            0u);

        constexpr std::uint32_t kSideQ8 = 38u;   // 0.1484375
        constexpr std::uint32_t kCenterQ8 = 180u; // 0.703125

        const auto resolveJobCountForRows =
            [&](int rows)
            {
                if (rows <= 0)
                {
                    return std::size_t{0u};
                }

                constexpr int kTargetRowsPerJob = 24;
                constexpr std::size_t kMaxResolveJobs = 8u;

                const std::size_t desiredJobs =
                    static_cast<std::size_t>(
                        std::max(
                            1,
                            (rows + kTargetRowsPerJob - 1) /
                                kTargetRowsPerJob));

                return std::min(
                    kMaxResolveJobs,
                    desiredJobs);
            };

        const auto runResolveRows =
            [&](std::size_t jobCount,
                const std::function<void(int, int)>& rowJob)
            {
                if (jobCount <= 1u ||
                    !traceJobExecutor_)
                {
                    rowJob(resolveMinY, resolveMaxY);
                    return;
                }

                traceJobExecutor_(
                    'X',
                    jobCount,
                    [&](std::size_t jobIndex, std::uint32_t /* workerId */)
                    {
                        const int firstRow =
                            resolveMinY +
                            static_cast<int>(
                                (static_cast<std::int64_t>(jobIndex) *
                                    static_cast<std::int64_t>(resolveRows)) /
                                static_cast<std::int64_t>(jobCount));

                        const int onePastLastRow =
                            resolveMinY +
                            static_cast<int>(
                                (static_cast<std::int64_t>(jobIndex + 1u) *
                                    static_cast<std::int64_t>(resolveRows)) /
                                static_cast<std::int64_t>(jobCount));

                        if (onePastLastRow <= firstRow)
                        {
                            return;
                        }

                        rowJob(firstRow, onePastLastRow - 1);
                    });
            };

        const std::size_t resolveJobCount =
            resolveJobCountForRows(resolveRows);

        const std::uint64_t resolveDispatchStartUs =
            static_cast<std::uint64_t>(
                frameTimer.nsecsElapsed() / 1000);

        recordPhase(
            'y',
            resolveSetupStartUs,
            resolveDispatchStartUs > resolveSetupStartUs
                ? resolveDispatchStartUs - resolveSetupStartUs
                : 0u);

        runResolveRows(
            resolveJobCount,
            [&](int firstRow, int lastRow)
            {
                for (int y = firstRow;
                    y <= lastRow;
                    ++y)
                {
                    const std::size_t sourceRow =
                        static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(resolveWidth);

                    const std::size_t targetRow =
                        static_cast<std::size_t>(y - resolveMinY) *
                        static_cast<std::size_t>(resolveSpan);

                    for (int x = resolveMinX;
                        x <= resolveMaxX;
                        ++x)
                    {
                        const int leftX =
                            std::max(
                                resolveMinX,
                                x - 1);

                        const int rightX =
                            std::min(
                                resolveMaxX,
                                x + 1);

                        const std::uint32_t left =
                            currentPhosphorEnergy[
                                sourceRow +
                                static_cast<std::size_t>(leftX)];

                        const std::uint32_t center =
                            currentPhosphorEnergy[
                                sourceRow +
                                static_cast<std::size_t>(x)];

                        const std::uint32_t right =
                            currentPhosphorEnergy[
                                sourceRow +
                                static_cast<std::size_t>(rightX)];

                        resolveHorizontal[
                            targetRow +
                            static_cast<std::size_t>(x - resolveMinX)] =
                            static_cast<std::uint16_t>(
                                (left * kSideQ8 +
                                 center * kCenterQ8 +
                                 right * kSideQ8 +
                                 128u) >> 8);
                    }
                }
            });

        runResolveRows(
            resolveJobCount,
            [&](int firstRow, int lastRow)
            {
                for (int y = firstRow;
                    y <= lastRow;
                    ++y)
                {
                    const int topY =
                        std::max(
                            resolveMinY,
                            y - 1);

                    const int bottomY =
                        std::min(
                            resolveMaxY,
                            y + 1);

                    const std::size_t topRow =
                        static_cast<std::size_t>(topY - resolveMinY) *
                        static_cast<std::size_t>(resolveSpan);

                    const std::size_t centerRow =
                        static_cast<std::size_t>(y - resolveMinY) *
                        static_cast<std::size_t>(resolveSpan);

                    const std::size_t bottomRow =
                        static_cast<std::size_t>(bottomY - resolveMinY) *
                        static_cast<std::size_t>(resolveSpan);

                    const std::size_t destinationRow =
                        static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(resolveWidth);

                    for (int x = resolveMinX;
                        x <= resolveMaxX;
                        ++x)
                    {
                        const std::size_t localX =
                            static_cast<std::size_t>(
                                x - resolveMinX);

                        const std::uint32_t top =
                            resolveHorizontal[
                                topRow + localX];

                        const std::uint32_t center =
                            resolveHorizontal[
                                centerRow + localX];

                        const std::uint32_t bottom =
                            resolveHorizontal[
                                bottomRow + localX];

                        currentPhosphorEnergy[
                            destinationRow +
                            static_cast<std::size_t>(x)] =
                            static_cast<std::uint16_t>(
                                (top * kSideQ8 +
                                 center * kCenterQ8 +
                                 bottom * kSideQ8 +
                                 128u) >> 8);
                    }
                }
            });

        phosphorMinX = resolveMinX;
        phosphorMaxX = resolveMaxX;
        phosphorMinY = resolveMinY;
        phosphorMaxY = resolveMaxY;
    }

    const std::uint64_t resolveUs =
        static_cast<std::uint64_t>(
            resolveTimer.nsecsElapsed() / 1000);

    traceLog(
        TraceEventType::WaveformResolve,
        logGeneration,
        0u,
        0u,
        resolveUs,
        (phosphorMaxX >= phosphorMinX && phosphorMaxY >= phosphorMinY) ? 1u : 0u,
        traceRendererId_);

    renderTimings_.glowUs =
        currentGlowUs;

    renderTimings_.traceRasterUs =
        currentCoreUs;

    renderTimings_.persistenceUs = 0;

    if (persistence_ > 0)
    {
        const std::uint64_t persistenceStartUs =
            static_cast<std::uint64_t>(
                frameTimer.nsecsElapsed() / 1000);

        phaseTimer.restart();

        applyScopephorFeedback(
            currentPhosphorEnergy,
            phosphorMinX,
            phosphorMinY,
            phosphorMaxX,
            phosphorMaxY);

        renderTimings_.persistenceUs =
            static_cast<std::uint64_t>(
                phaseTimer.nsecsElapsed() / 1000);

        recordPhase(
            'P',
            persistenceStartUs,
            renderTimings_.persistenceUs);

        traceLog(
            TraceEventType::WaveformPersistence,
            logGeneration,
            0u,
            0u,
            renderTimings_.persistenceUs,
            static_cast<std::uint64_t>(persistence_),
            traceRendererId_);
    }

    const std::uint64_t baseClearStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();

    // image_ is only the final Qt transport surface. Phosphor math is raw.
    // Acquire a detached, already-allocated transport surface before clearing.
    // This avoids QImage copy-on-write detaching the published previous frame.
    prepareImageForRender();

    // Delta 70: B is pure row-owned memory clear work.  Spread it over the
    // existing WF/V1/V2/VS assist queue instead of making the waveform thread
    // clear the complete transport surface serially.  prepareImageForRender()
    // stays serial because allocation/detach must complete before any worker
    // receives a scanline pointer.
    const int baseClearHeight = image_.height();
    constexpr int kBaseClearRowsPerJob = 64;
    constexpr std::size_t kMaxBaseClearJobs = 8u;
    const std::size_t baseClearJobCount =
        baseClearHeight > 0
        ? std::min<std::size_t>(
            kMaxBaseClearJobs,
            static_cast<std::size_t>(
                std::max(
                    1,
                    (baseClearHeight + kBaseClearRowsPerJob - 1) /
                        kBaseClearRowsPerJob)))
        : 0u;

    const auto clearBaseRows =
        [&](std::size_t jobIndex, std::uint32_t /* workerId */)
        {
            if (baseClearJobCount == 0u ||
                jobIndex >= baseClearJobCount)
            {
                return;
            }

            const int firstY =
                static_cast<int>(
                    (static_cast<std::int64_t>(jobIndex) * baseClearHeight) /
                    static_cast<std::int64_t>(baseClearJobCount));
            const int onePastLastY =
                static_cast<int>(
                    (static_cast<std::int64_t>(jobIndex + 1u) * baseClearHeight) /
                    static_cast<std::int64_t>(baseClearJobCount));

            const qsizetype bytesPerLine = image_.bytesPerLine();
            for (int y = firstY; y < onePastLastY; ++y)
            {
                std::memset(
                    image_.scanLine(y),
                    0,
                    static_cast<std::size_t>(bytesPerLine));
            }
        };

    if (baseClearJobCount > 1u && traceJobExecutor_)
    {
        traceJobExecutor_('B', baseClearJobCount, clearBaseRows);
    }
    else if (baseClearJobCount == 1u)
    {
        clearBaseRows(0u, 0u);
    }

    renderTimings_.baseClearUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    recordPhase(
        'B',
        baseClearStartUs,
        renderTimings_.baseClearUs);

    const std::uint64_t graticuleStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();

    // Draw the graticule first. Signal traces are composed afterwards so
    // the beam remains visually in front of the graticule.
    {
        QPainter graticulePainter(&image_);
        graticule_.draw(
            graticulePainter,
            scope,
            VideoStandard::pal625());
    }

    renderTimings_.graticuleUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    recordPhase(
        'G',
        graticuleStartUs,
        renderTimings_.graticuleUs);

    if (chromaFillIntensity_ > 0 &&
        !measurementProbePresentation_)
    {
        /*
         * Chroma is deliberately NOT part of Scopephor.
         *
         * Restore the pre-Scopephor current-frame chroma renderer verbatim in
         * spirit:
         *   - current U/V only
         *   - old envelope geometry
         *   - old RGB derivation
         *   - direct vertical RGB spans using the same display LUT
         *
         * Chroma is written directly into the current target. It therefore
         * has no persistence, no age weighting and no glow.
         */
        const std::uint64_t composeStartUs =
            static_cast<std::uint64_t>(
                frameTimer.nsecsElapsed() / 1000);
        phaseTimer.restart();

        const int displayWidth =
            image_.width();

        std::vector<double> chromaUpperY(
            static_cast<std::size_t>(displayWidth));

        std::vector<double> chromaLowerY(
            static_cast<std::size_t>(displayWidth));

        std::vector<int> chromaRed(
            static_cast<std::size_t>(displayWidth));

        std::vector<int> chromaGreen(
            static_cast<std::size_t>(displayWidth));

        std::vector<int> chromaBlue(
            static_cast<std::size_t>(displayWidth));

        const double voltsToPixels =
            scope.height() /
            analog.graticuleMaxVolts;

        const double chromaXScale =
            scope.width() /
            static_cast<double>(
                std::max(1, displayWidth - 1));

        for (int x = 0;
            x < displayWidth;
            ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(x);

            const double yValue =
                std::clamp(
                    static_cast<double>(
                        displayY_[index]),
                    0.0,
                    kMaximumSampleValue);

            const double uValue =
                std::clamp(
                    static_cast<double>(
                        displayU_[index]),
                    0.0,
                    kMaximumSampleValue);

            const double vValue =
                std::clamp(
                    static_cast<double>(
                        displayV_[index]),
                    0.0,
                    kMaximumSampleValue);

            const double neutralChroma =
                static_cast<double>(
                    digitalLevels.chromaNeutral) *
                64.0;

            const double chromaNoiseThreshold =
                neutralChroma *
                kChromaNoiseThresholdFraction;

            double chromaU =
                uValue -
                neutralChroma;

            double chromaV =
                vValue -
                neutralChroma;

            const auto palChroma =
                palChromaCoefficients(
                    VideoColorStandard::Rec601_625);

            const double palU =
                chromaU *
                palChroma.cbToU;

            const double palV =
                chromaV *
                palChroma.crToV;

            const double chromaMagnitude =
                std::hypot(
                    palU,
                    palV);

            if (chromaMagnitude <
                chromaNoiseThreshold)
            {
                chromaU = 0.0;
                chromaV = 0.0;
            }

            const double plotY =
                scope.bottom() -
                yValue *
                voltsToPixels;

            const double nominalChromaExcursion =
                static_cast<double>(
                    digitalLevels.chromaPositiveExcursion) *
                64.0;

            const double chromaVolts =
                std::hypot(
                    palU,
                    palV) *
                analog.chromaPeakVolts /
                nominalChromaExcursion;

            const double spread =
                chromaVolts *
                voltsToPixels;

            chromaUpperY[index] =
                plotY - spread;

            chromaLowerY[index] =
                plotY + spread;

            const auto rgb =
                yuvToRgbCoefficients(
                    VideoColorStandard::Rec601_625);

            double redValue =
                rgb.rCr * chromaV;

            double greenValue =
                rgb.gCb * chromaU +
                rgb.gCr * chromaV;

            double blueValue =
                rgb.bCb * chromaU;

            const double minimum =
                std::min({
                    redValue,
                    greenValue,
                    blueValue
                    });

            redValue -= minimum;
            greenValue -= minimum;
            blueValue -= minimum;

            const double maximum =
                std::max({
                    redValue,
                    greenValue,
                    blueValue
                    });

            int red = 0;
            int green = 0;
            int blue = 0;

            if (maximum > 0.0)
            {
                red =
                    static_cast<int>(
                        std::clamp(
                            redValue *
                            255.0 /
                            maximum,
                            0.0,
                            255.0));

                green =
                    static_cast<int>(
                        std::clamp(
                            greenValue *
                            255.0 /
                            maximum,
                            0.0,
                            255.0));

                blue =
                    static_cast<int>(
                        std::clamp(
                            blueValue *
                            255.0 /
                            maximum,
                            0.0,
                            255.0));
            }

            chromaRed[index] = red;
            chromaGreen[index] = green;
            chromaBlue[index] = blue;
        }

        const int firstScreenX =
            std::clamp(
                static_cast<int>(
                    std::ceil(scope.left())),
                0,
                image_.width());

        const int lastScreenXExclusive =
            std::clamp(
                static_cast<int>(
                    std::floor(scope.right())) + 1,
                firstScreenX,
                image_.width());

        for (int screenX = firstScreenX;
            screenX < lastScreenXExclusive;
            ++screenX)
        {
            const double normalisedX =
                (static_cast<double>(screenX) -
                    scope.left()) /
                scope.width();

            const std::size_t index =
                std::min(
                    static_cast<std::size_t>(
                        std::clamp(
                            normalisedX,
                            0.0,
                            1.0) *
                        static_cast<double>(
                            displayWidth - 1)),
                    static_cast<std::size_t>(
                        displayWidth - 1));

            const int firstY =
                std::clamp(
                    static_cast<int>(
                        std::ceil(
                            chromaUpperY[index])),
                    0,
                    image_.height() - 1);

            const int lastY =
                std::clamp(
                    static_cast<int>(
                        std::floor(
                            chromaLowerY[index])),
                    0,
                    image_.height() - 1);

            if (lastY < firstY)
            {
                continue;
            }

            const auto chromaLevel =
                [this](int channel) -> int
                {
                    const std::uint32_t contribution =
                        static_cast<std::uint32_t>(
                            std::clamp(
                                channel,
                                0,
                                255)) *
                        static_cast<std::uint32_t>(
                            chromaFillIntensity_) /
                        255u;

                    return displayLut_[
                        static_cast<std::uint16_t>(
                            std::min<std::uint32_t>(
                                kMaximumChromaLevel,
                                contribution))];
                };

            const int redContribution =
                chromaLevel(
                    chromaRed[index]);

            const int greenContribution =
                chromaLevel(
                    chromaGreen[index]);

            const int blueContribution =
                chromaLevel(
                    chromaBlue[index]);

            const int monoContribution =
                std::max({
                    redContribution,
                    greenContribution,
                    blueContribution
                    });

            for (int y = firstY;
                y <= lastY;
                ++y)
            {
                auto* destination =
                    reinterpret_cast<QRgb*>(
                        image_.scanLine(y));

                if (settings_.color)
                {
                    destination[screenX] =
                        qRgb(
                            redContribution,
                            greenContribution,
                            blueContribution);
                }
                else
                {
                    destination[screenX] =
                        qRgb(
                            monoContribution,
                            monoContribution,
                            monoContribution);
                }
            }
        }

        // Chroma was written directly as vertical spans above.
        renderTimings_.composeUs =
            static_cast<std::uint64_t>(
                phaseTimer.nsecsElapsed() / 1000);
        recordPhase(
            'C',
            composeStartUs,
            renderTimings_.composeUs);
    }
    else
    {
        renderTimings_.composeUs = 0u;
    }

    traceLog(
        TraceEventType::WaveformCompose,
        logGeneration,
        0u,
        0u,
        renderTimings_.composeUs,
        static_cast<std::uint64_t>(chromaFillIntensity_),
        traceRendererId_);

    // Raw phosphor energy -> QImage transport surface.
    // No QPainter and no Qt image blending.
    const std::uint64_t phosphorComposeStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();

    const int phosphorWidth =
        image_.width();

    const int phosphorHeight =
        image_.height();

    if (currentPhosphorEnergy.size() ==
            static_cast<std::size_t>(phosphorWidth) *
            static_cast<std::size_t>(phosphorHeight) &&
        phosphorMaxX >= phosphorMinX &&
        phosphorMaxY >= phosphorMinY)
    {
        phosphorMinX = std::clamp(phosphorMinX, 0, phosphorWidth - 1);
        phosphorMaxX = std::clamp(phosphorMaxX, 0, phosphorWidth - 1);
        phosphorMinY = std::clamp(phosphorMinY, 0, phosphorHeight - 1);
        phosphorMaxY = std::clamp(phosphorMaxY, 0, phosphorHeight - 1);

        const bool probePresentation =
            measurementProbePresentation_.load(
                std::memory_order_relaxed);

        double probeCenterX = 0.0;
        double probeCenterY = 0.0;
        double probeRadiusSquared = 0.0;

        if (probePresentation)
        {
            const AnalogVideoLevels probeAnalog =
                analogLevels(VideoColorStandard::Rec601_625);

            const double probeNormalizedX =
                measurementProbeNormalizedX_.load(
                    std::memory_order_relaxed);

            const double probeVolts =
                measurementProbeVolts_.load(
                    std::memory_order_relaxed);

            probeCenterX =
                scope.left() +
                probeNormalizedX * scope.width();

            probeCenterY =
                scope.bottom() -
                probeVolts * scope.height() /
                    probeAnalog.graticuleMaxVolts;

            const double videoHeight =
                std::abs(
                    (probeAnalog.whiteVolts - probeAnalog.blackVolts) *
                    scope.height() /
                    probeAnalog.graticuleMaxVolts);

            const double probeDiameter =
                (std::max)(12.0, videoHeight * 0.10);

            const double probeRadius =
                probeDiameter * 0.5;

            probeRadiusSquared =
                probeRadius * probeRadius;
        }

        const bool colorizeIllegalLuminance =
            colorizeIllegalLuminance_.load(std::memory_order_acquire);
        constexpr double kIllegalLowVolts = 0.280;
        constexpr double kIllegalHighVolts = 1.020;
        const AnalogVideoLevels legalAnalog =
            analogLevels(VideoColorStandard::Rec601_625);

        // Keep legality presentation linear and symmetric around nominal
        // black/white.  PAL black is 0.300 V and white is 1.000 V; the
        // selected limits 0.280 V and 1.020 V are therefore exactly 20 mV
        // outside either end.  Older builds carried an extra lower-only beam
        // footprint guard dating from the former near-black limit; retaining that
        // guard after moving to 0.280 V made the visible margins asymmetric.

        // Delta 70: Q owns independent destination rows, so resolve the
        // phosphor energy into RGB over the same assist queue.  All probe and
        // legality state above is immutable for the duration of the jobs.
        const int phosphorRows =
            std::max(0, phosphorMaxY - phosphorMinY + 1);
        constexpr int kPhosphorRowsPerJob = 48;
        constexpr std::size_t kMaxPhosphorJobs = 8u;
        const std::size_t phosphorJobCount =
            phosphorRows > 0
            ? std::min<std::size_t>(
                kMaxPhosphorJobs,
                static_cast<std::size_t>(
                    std::max(
                        1,
                        (phosphorRows + kPhosphorRowsPerJob - 1) /
                            kPhosphorRowsPerJob)))
            : 0u;

        const auto composePhosphorRows =
            [&](std::size_t jobIndex, std::uint32_t /* workerId */)
            {
                if (phosphorJobCount == 0u ||
                    jobIndex >= phosphorJobCount)
                {
                    return;
                }

                const int firstY =
                    phosphorMinY +
                    static_cast<int>(
                        (static_cast<std::int64_t>(jobIndex) * phosphorRows) /
                        static_cast<std::int64_t>(phosphorJobCount));
                const int onePastLastY =
                    phosphorMinY +
                    static_cast<int>(
                        (static_cast<std::int64_t>(jobIndex + 1u) * phosphorRows) /
                        static_cast<std::int64_t>(phosphorJobCount));

                for (int y = firstY;
                    y < onePastLastY;
                    ++y)
                {
                    auto* destination =
                        reinterpret_cast<QRgb*>(
                            image_.scanLine(y));

                    const std::size_t row =
                        static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(
                            phosphorWidth);

                    const double rowVolts =
                        (scope.bottom() - (static_cast<double>(y) + 0.5)) *
                        legalAnalog.graticuleMaxVolts /
                        (std::max)(scope.height(), 1.0);

                    const bool illegalLuminanceRow =
                        colorizeIllegalLuminance &&
                        (rowVolts < kIllegalLowVolts ||
                         rowVolts > kIllegalHighVolts);

                    for (int x = phosphorMinX;
                        x <= phosphorMaxX;
                        ++x)
                    {
                        const std::uint16_t energy =
                            currentPhosphorEnergy[
                                row +
                                static_cast<std::size_t>(x)];

                        if (energy == 0u)
                        {
                            continue;
                        }

                        // Q-less display gain.  The previous >>8 mapping made the
                        // 16-bit beam energy almost black (255*7 -> only ~6).
                        // >>3 restores a hot white core while preserving headroom
                        // for glow and phosphor accumulation.
                        const int tracePeakWhite =
                            lineInfoOverlayPalOutput_
                            ? 191
                            : 255;

                        int value =
                            std::min(
                                tracePeakWhite,
                                static_cast<int>(
                                    energy >> 3));

                        if (probePresentation)
                        {
                            const double dx =
                                (static_cast<double>(x) + 0.5) -
                                probeCenterX;

                            const double dy =
                                (static_cast<double>(y) + 0.5) -
                                probeCenterY;

                            const bool insideProbe =
                                dx * dx + dy * dy <=
                                probeRadiusSquared;

                            // Dim the context, but keep the trace inside the
                            // measurement circle at its normal calibrated strength.
                            if (!insideProbe)
                            {
                                value = (value + 1) / 2;
                            }
                        }

                        const QRgb old =
                            destination[x];

                        if (illegalLuminanceRow)
                        {
                            // Legality colour is applied while resolving ScopePhor
                            // energy to RGB, so both the current beam and retained
                            // phosphor history stay red outside the legal Y range.
                            destination[x] =
                                qRgb(
                                    std::min(
                                        255,
                                        qRed(old) + value),
                                    0,
                                    0);
                        }
                        else
                        {
                            destination[x] =
                                qRgb(
                                    std::min(
                                        255,
                                        qRed(old) + value),
                                    std::min(
                                        255,
                                        qGreen(old) + value),
                                    std::min(
                                        255,
                                        qBlue(old) + value));
                        }
                    }
                }
            };

        if (phosphorJobCount > 1u && traceJobExecutor_)
        {
            traceJobExecutor_('Q', phosphorJobCount, composePhosphorRows);
        }
        else if (phosphorJobCount == 1u)
        {
            composePhosphorRows(0u, 0u);
        }
    }

    const std::uint64_t phosphorComposeUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    renderTimings_.phosphorComposeUs =
        phosphorComposeUs;

    // Illegal luminance is colourized during ScopePhor RGB composition
    // above.  Doing it there (rather than as a post-overlay) keeps retained
    // phosphor history the same red as the live beam.

    recordPhase(
        'Q',
        phosphorComposeStartUs,
        phosphorComposeUs);

    traceLog(
        TraceEventType::WaveformPersistence,
        logGeneration,
        0u,
        1u,
        phosphorComposeUs,
        renderTimings_.persistenceUs,
        traceRendererId_);


    // Luma Scopephor was already composed before the current-frame chroma
    // overlay.  Do not re-render any historical beam geometry here.
    renderTimings_.traceUs =
        renderTimings_.tracePrepUs +
        renderTimings_.traceRasterUs;
    renderTimings_.traceParallel =
        catWuzleFrameStats.chunkCount > 1u;
    renderTimings_.traceJobCount =
        std::max<std::uint32_t>(1u, catWuzleFrameStats.chunkCount);

    const std::uint64_t overlayStartUs =
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000);
    phaseTimer.restart();
    QPainter overlayPainter(&image_);
    drawLineInfoOverlay(overlayPainter);
    renderTimings_.overlayUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);
    recordPhase(
        'O',
        overlayStartUs,
        renderTimings_.overlayUs);

    traceLog(
        TraceEventType::WaveformCompose,
        logGeneration,
        0u,
        1u,
        renderTimings_.overlayUs,
        0u,
        traceRendererId_);

    traceLog(
        TraceEventType::WaveformEnd,
        logGeneration,
        0u,
        0u,
        static_cast<std::uint64_t>(
            frameTimer.nsecsElapsed() / 1000),
        renderTimings_.traceUs,
        traceRendererId_);
}



std::vector<WaveformRenderer::BeamPoint> WaveformRenderer::buildCurrentLumaPolyline(
    const QRectF& scope,
    std::size_t viewOffset,
    std::size_t viewWidth) const
{
    std::vector<BeamPoint> polyline;

    if (viewWidth < 2u ||
        fullLumaVolts_.size() <
            viewOffset + viewWidth)
    {
        return polyline;
    }

    const auto analog =
        analogLevels(
            VideoColorStandard::Rec601_625);

    const double voltsToPixels =
        scope.height() /
        analog.graticuleMaxVolts;

    const double xScale =
        scope.width() /
        static_cast<double>(
            viewWidth - 1u);

    const auto pointAt =
        [this,
         viewOffset,
         viewWidth,
         &scope,
         voltsToPixels,
         xScale](std::int64_t sample) -> BeamPoint
        {
            sample =
                std::clamp<std::int64_t>(
                    sample,
                    0,
                    static_cast<std::int64_t>(
                        viewWidth) - 1);

            const std::size_t index =
                viewOffset +
                static_cast<std::size_t>(
                    sample);

            return BeamPoint
            {
                scope.left() +
                    static_cast<double>(sample) *
                    xScale,

                scope.bottom() -
                    static_cast<double>(
                        fullLumaVolts_[index]) *
                    voltsToPixels
            };
        };

    const auto catmull =
        [](const BeamPoint& p0,
           const BeamPoint& p1,
           const BeamPoint& p2,
           const BeamPoint& p3,
           double t) -> BeamPoint
        {
            const double t2 = t * t;
            const double t3 = t2 * t;

            return BeamPoint
            {
                0.5 *
                ((2.0 * p1.x) +
                 (-p0.x + p2.x) * t +
                 (2.0 * p0.x -
                  5.0 * p1.x +
                  4.0 * p2.x -
                  p3.x) * t2 +
                 (-p0.x +
                  3.0 * p1.x -
                  3.0 * p2.x +
                  p3.x) * t3),

                0.5 *
                ((2.0 * p1.y) +
                 (-p0.y + p2.y) * t +
                 (2.0 * p0.y -
                  5.0 * p1.y +
                  4.0 * p2.y -
                  p3.y) * t2 +
                 (-p0.y +
                  3.0 * p1.y -
                  3.0 * p2.y +
                  p3.y) * t3)
            };
        };

    polyline.reserve(
        viewWidth * 2u);

    polyline.push_back(
        pointAt(0));

    for (std::size_t i = 0u;
        i + 1u < viewWidth;
        ++i)
    {
        const BeamPoint p0 =
            pointAt(
                static_cast<std::int64_t>(i) - 1);

        const BeamPoint p1 =
            pointAt(
                static_cast<std::int64_t>(i));

        const BeamPoint p2 =
            pointAt(
                static_cast<std::int64_t>(i) + 1);

        const BeamPoint p3 =
            pointAt(
                static_cast<std::int64_t>(i) + 2);

        polyline.push_back(
            catmull(
                p0,
                p1,
                p2,
                p3,
                0.5));

        polyline.push_back(p2);
    }

    return polyline;
}

std::vector<bool> WaveformRenderer::buildDenseSteepPacketMask(
    const std::vector<BeamPoint>& polyline,
    DenseSteepStats* stats) const
{
    if (stats != nullptr)
    {
        *stats = {};
    }

    if (polyline.size() < 2u)
    {
        return {};
    }

    /*
     * MUD latch detector.
     *
     * The expensive neighbour proof is done once per monotone flank, not for
     * every Catmull segment.  RED keeps looking for permission to become
     * WHITE; once a flank is proven dense, WHITE is latched until the sign of
     * dY changes at the next top/bottom.  This is the important asymmetry:
     * after WHITE there is nothing useful left to prove on the same flank.
     *
     * Packet acceptance stays conservative: a dense seed still needs nearby
     * sustained flanks on both sides, and the complete packet must meet the
     * existing run-count and X-width thresholds.  Accepted packet edge flanks
     * are included, exactly as in ZEEF2/POLYGRAPH.
     */

    struct MonotoneRun
    {
        std::size_t firstSegment = 0u;
        std::size_t lastSegment = 0u;
        double minX = 0.0;
        double maxX = 0.0;
        double minY = 0.0;
        double maxY = 0.0;
        double totalAbsDx = 0.0;
        double totalAbsDy = 0.0;
        int direction = 0;
        bool hasEntrySegment = false;
        bool sustained = false;
        bool dense = false;
    };

    const std::size_t segmentCount = polyline.size() - 1u;

    constexpr double kDirectionEpsilonPx = 0.05;
    constexpr double kMinimumSegmentDyPx = 10.0;
    constexpr double kMinimumSteepSlope = 2.75;
    constexpr double kMinimumRunVerticalTravelPx = 12.0;
    constexpr double kMinimumRunVerticalSpanPx = 10.0;
    constexpr double kMinimumRunSlope = 3.5;
    constexpr std::size_t kMinimumRunSegments = 3u;

    const auto directionOf =
        [&](const std::size_t segment) -> int
        {
            const double dy =
                polyline[segment + 1u].y - polyline[segment].y;

            if (dy > kDirectionEpsilonPx)
            {
                return 1;
            }
            if (dy < -kDirectionEpsilonPx)
            {
                return -1;
            }
            return 0;
        };

    std::vector<MonotoneRun> runs;
    runs.reserve(segmentCount / 2u + 1u);

    std::size_t runStart = 0u;
    int currentDirection = 0;

    for (std::size_t i = 0u; i < segmentCount; ++i)
    {
        const int direction = directionOf(i);

        if (currentDirection == 0 && direction != 0)
        {
            currentDirection = direction;
        }

        const bool flipsDirection =
            direction != 0 &&
            currentDirection != 0 &&
            direction != currentDirection;

        if (!flipsDirection)
        {
            continue;
        }

        MonotoneRun run;
        run.firstSegment = runStart;
        run.lastSegment = i - 1u;
        run.direction = currentDirection;
        runs.push_back(run);

        runStart = i;
        currentDirection = direction;
    }

    MonotoneRun finalRun;
    finalRun.firstSegment = runStart;
    finalRun.lastSegment = segmentCount - 1u;
    finalRun.direction = currentDirection;
    runs.push_back(finalRun);

    if (stats != nullptr)
    {
        stats->runCount = static_cast<std::uint32_t>(runs.size());
    }

    // Summarise each monotone flank once.  dY >= 10 px is only permission to
    // enter WHITE; low-dY top/bottom segments remain part of the flank.
    for (MonotoneRun& run : runs)
    {
        const BeamPoint& firstPoint = polyline[run.firstSegment];
        run.minX = firstPoint.x;
        run.maxX = firstPoint.x;
        run.minY = firstPoint.y;
        run.maxY = firstPoint.y;

        for (std::size_t segment = run.firstSegment;
             segment <= run.lastSegment;
             ++segment)
        {
            const BeamPoint& a = polyline[segment];
            const BeamPoint& b = polyline[segment + 1u];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double absDx = std::abs(dx);
            const double absDy = std::abs(dy);

            run.totalAbsDx += absDx;
            run.totalAbsDy += absDy;
            run.minX = std::min(run.minX, std::min(a.x, b.x));
            run.maxX = std::max(run.maxX, std::max(a.x, b.x));
            run.minY = std::min(run.minY, std::min(a.y, b.y));
            run.maxY = std::max(run.maxY, std::max(a.y, b.y));

            if (!run.hasEntrySegment)
            {
                const double slope = absDy / std::max(0.05, absDx);
                if (absDy >= kMinimumSegmentDyPx &&
                    slope >= kMinimumSteepSlope)
                {
                    run.hasEntrySegment = true;
                }
            }
        }

        const double verticalSpan = run.maxY - run.minY;
        const double runSlope =
            run.totalAbsDy / std::max(0.20, run.totalAbsDx);
        const std::size_t runSegmentCount =
            run.lastSegment - run.firstSegment + 1u;

        run.sustained =
            run.direction != 0 &&
            run.hasEntrySegment &&
            runSegmentCount >= kMinimumRunSegments &&
            run.totalAbsDy >= kMinimumRunVerticalTravelPx &&
            verticalSpan >= kMinimumRunVerticalSpanPx &&
            runSlope >= kMinimumRunSlope;
    }

    if (stats != nullptr)
    {
        for (const MonotoneRun& run : runs)
        {
            if (run.sustained)
            {
                ++stats->sustainedRunCount;
            }
        }
    }

    constexpr double kNeighbourMinDistancePx = 0.75;
    constexpr double kNeighbourMaxDistancePx = 8.0;
    constexpr double kMinimumYOverlapRatio = 0.55;

    const auto runCenterX =
        [](const MonotoneRun& run)
        {
            return 0.5 * (run.minX + run.maxX);
        };

    const auto yOverlapIsEnough =
        [&](const MonotoneRun& a, const MonotoneRun& b)
        {
            const double overlapY = std::max(
                0.0,
                std::min(a.maxY, b.maxY) -
                std::max(a.minY, b.minY));
            const double aHeight = std::max(1.0, a.maxY - a.minY);
            const double bHeight = std::max(1.0, b.maxY - b.minY);
            const double overlapRatio =
                overlapY / std::min(aHeight, bHeight);

            return overlapRatio >= kMinimumYOverlapRatio;
        };

    /*
     * The runs are generated in waveform/X order.  Therefore a neighbour can
     * only live in the small local X window around the current run.  Walk
     * outward and STOP as soon as the X distance exceeds 8 px.  This removes
     * the old all-runs-against-all-runs search.
     *
     * Once both sides are proven, the run is WHITE-latched and no more
     * neighbour work is done for that monotone flank.  The next run exists
     * only after dY has changed sign, so that is naturally the reset point.
     */
    for (std::size_t i = 0u; i < runs.size(); ++i)
    {
        MonotoneRun& run = runs[i];
        if (!run.sustained)
        {
            continue;
        }

        const double centerX = runCenterX(run);
        bool neighbourLeft = false;
        bool neighbourRight = false;

        for (std::size_t j = i; j-- > 0u;)
        {
            const MonotoneRun& other = runs[j];
            if (stats != nullptr)
            {
                ++stats->neighbourProbes;
            }
            const double distanceX = centerX - runCenterX(other);

            if (distanceX > kNeighbourMaxDistancePx)
            {
                break;
            }
            if (distanceX < kNeighbourMinDistancePx ||
                !other.sustained ||
                !yOverlapIsEnough(run, other))
            {
                continue;
            }

            neighbourLeft = true;
            break;
        }

        for (std::size_t j = i + 1u; j < runs.size(); ++j)
        {
            const MonotoneRun& other = runs[j];
            if (stats != nullptr)
            {
                ++stats->neighbourProbes;
            }
            const double distanceX = runCenterX(other) - centerX;

            if (distanceX > kNeighbourMaxDistancePx)
            {
                break;
            }
            if (distanceX < kNeighbourMinDistancePx ||
                !other.sustained ||
                !yOverlapIsEnough(run, other))
            {
                continue;
            }

            neighbourRight = true;
            break;
        }

        run.dense = neighbourLeft && neighbourRight;
        if (run.dense && stats != nullptr)
        {
            ++stats->denseRunCount;
        }
    }

    constexpr double kMinimumDensePacketWidthPx = 14.0;
    constexpr std::size_t kMinimumDensePacketRuns = 5u;

    const auto runsArePacketNeighbours =
        [&](const MonotoneRun& left, const MonotoneRun& right)
        {
            if (!left.sustained || !right.sustained)
            {
                return false;
            }

            const double distanceX =
                std::abs(runCenterX(right) - runCenterX(left));

            return
                distanceX >= kNeighbourMinDistancePx &&
                distanceX <= kNeighbourMaxDistancePx &&
                yOverlapIsEnough(left, right);
        };

    std::vector<bool> packetRun(runs.size(), false);
    std::vector<bool> denseSteepSegment(segmentCount, false);

    for (std::size_t seed = 0u; seed < runs.size(); ++seed)
    {
        if (!runs[seed].dense || packetRun[seed])
        {
            continue;
        }

        std::size_t firstRun = seed;
        std::size_t lastRun = seed;

        while (firstRun > 0u &&
               runsArePacketNeighbours(runs[firstRun - 1u], runs[firstRun]))
        {
            --firstRun;
        }

        while (lastRun + 1u < runs.size() &&
               runsArePacketNeighbours(runs[lastRun], runs[lastRun + 1u]))
        {
            ++lastRun;
        }

        const std::size_t packetRuns = lastRun - firstRun + 1u;
        double packetMinX = runs[firstRun].minX;
        double packetMaxX = runs[firstRun].maxX;
        bool containsDenseSeed = false;

        for (std::size_t runIndex = firstRun; runIndex <= lastRun; ++runIndex)
        {
            packetMinX = std::min(packetMinX, runs[runIndex].minX);
            packetMaxX = std::max(packetMaxX, runs[runIndex].maxX);
            containsDenseSeed = containsDenseSeed || runs[runIndex].dense;
        }

        const bool packetAccepted =
            containsDenseSeed &&
            packetRuns >= kMinimumDensePacketRuns &&
            (packetMaxX - packetMinX) >= kMinimumDensePacketWidthPx;

        if (!packetAccepted)
        {
            continue;
        }

        if (stats != nullptr)
        {
            ++stats->acceptedPacketCount;
        }

        for (std::size_t runIndex = firstRun; runIndex <= lastRun; ++runIndex)
        {
            packetRun[runIndex] = true;
        }

        // Whole monotone flanks are latched WHITE.  Tops/bottoms only reset
        // the detector because they start the next run; there is no repeated
        // neighbour test inside an already accepted flank.
        const std::size_t firstSegment = runs[firstRun].firstSegment;
        const std::size_t lastSegment = runs[lastRun].lastSegment;
        for (std::size_t segment = firstSegment;
             segment <= lastSegment;
             ++segment)
        {
            denseSteepSegment[segment] = true;
        }
    }

    if (stats != nullptr)
    {
        stats->whiteSegmentCount = static_cast<std::uint32_t>(
            std::count(denseSteepSegment.begin(), denseSteepSegment.end(), true));
    }

    return denseSteepSegment;
}

void WaveformRenderer::renderCurrentPhosphorEnergy(
    const std::vector<BeamPoint>& polyline,
    const std::vector<bool>& denseSteepSegment,
    const QRectF& plotRect,
    std::uint64_t& glowUs,
    std::uint64_t& coreUs,
    int& activeMinX,
    int& activeMinY,
    int& activeMaxX,
    int& activeMaxY,
    CatWuzleFrameStats& frameStats,
    std::uint64_t timelineBaseUs)
{
    (void)plotRect;
    frameStats = {};
    glowUs = 0u;
    coreUs = 0u;

    // Delta 41: sub-phases inside this helper need the same capture-relative
    // clock as the parent render.  Keep a local elapsed clock and add the
    // caller-supplied base instead of referring to renderSingleLine locals.
    QElapsedTimer phaseClock;
    phaseClock.start();

    const auto recordLocalPhase =
        [this](char label,
            std::uint64_t startUs,
            std::uint64_t durationUs)
        {
            if (durationUs == 0u ||
                renderTimings_.phaseCount >=
                    WaveformRenderTimings::kPhaseCapacity)
            {
                return;
            }

            auto& event =
                renderTimings_.phases[
                    renderTimings_.phaseCount++];
            event.label = label;
            event.startUs = startUs;
            event.durationUs = durationUs;
        };

    const int width =
        image_.width();

    const int height =
        image_.height();

    if (polyline.size() < 2u ||
        width <= 0 ||
        height <= 0)
    {
        currentPhosphorEnergy_.clear();
        currentPhosphorEnergyWidth_ = 0;
        currentPhosphorEnergyHeight_ = 0;
        return;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);

    // Persistent monochrome 16-bit beam-energy target. Allocation is paid only
    // when the target resolution changes. E is resize/reallocation; lower-case
    // e is the steady-state clear. A resolution change gets fresh zeroed storage
    // and therefore does not immediately clear the same memory a second time.
    const bool energyTargetChanged =
        currentPhosphorEnergyWidth_ != width ||
        currentPhosphorEnergyHeight_ != height ||
        currentPhosphorEnergy_.size() != pixelCount;

    if (energyTargetChanged)
    {
        const std::uint64_t allocationStartUs =
            timelineBaseUs +
            static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
        QElapsedTimer allocationTimer;
        allocationTimer.start();

        std::vector<std::uint16_t> freshEnergy(pixelCount, 0u);
        currentPhosphorEnergy_.swap(freshEnergy);
        currentPhosphorEnergyWidth_ = width;
        currentPhosphorEnergyHeight_ = height;

        const std::uint64_t allocationUs =
            static_cast<std::uint64_t>(allocationTimer.nsecsElapsed() / 1000);
        recordLocalPhase('E', allocationStartUs, allocationUs);
    }
    else
    {
        const std::uint64_t clearStartUs =
            timelineBaseUs +
            static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
        QElapsedTimer clearTimer;
        clearTimer.start();

        std::fill(
            currentPhosphorEnergy_.begin(),
            currentPhosphorEnergy_.end(),
            std::uint16_t{0});

        const std::uint64_t clearUs =
            static_cast<std::uint64_t>(clearTimer.nsecsElapsed() / 1000);
        recordLocalPhase('e', clearStartUs, clearUs);
    }

    auto& currentEnergy = currentPhosphorEnergy_;

    const double renderDimension =
        static_cast<double>(
            std::max(
                1,
                std::min(
                    width,
                    height)));

    const double beamScale =
        std::clamp(
            std::sqrt(
                renderDimension /
                576.0),
            1.0,
            1.90);

    const double glowScale =
        static_cast<double>(
            std::clamp(glow_, 0, 100)) /
        100.0;

    /*
     * CATWUZLE ANALYTIC CORE + LOCAL GLOW
     * ------------------------------------
     *
     * The white beam core is NOT made from point stamps anymore.
     *
     * Every Catweazle polyline segment is rasterised directly against the
     * target pixel grid using the shortest distance from a target pixel
     * centre to the actual segment.  A one-pixel analytic coverage ramp
     * provides subpixel antialiasing continuously for every possible angle.
     *
     * This removes the two weaknesses that became visible after thinning the
     * large-target beam:
     *
     *   - phase/angle "stairs" on steep slopes
     *   - occasional holes between one-pixel-spaced point stamps
     *
     * Beam Glow remains deliberately cheap: it is still a small local 5x5
     * radial stamp sampled along the trace.  Glow is garnish; the visible
     * white core now comes from the analytic line rasteriser.
     */
    constexpr int kGlowKernelSize = 5;
    constexpr int kGlowKernelRadius =
        kGlowKernelSize / 2;

    constexpr int kSubpixelPhases = 8;
    constexpr int kSubpixelKernelCount =
        kSubpixelPhases * kSubpixelPhases;

    using GlowKernel =
        std::array<std::uint16_t,
            kGlowKernelSize * kGlowKernelSize>;

    const double linearTargetScale =
        std::clamp(
            static_cast<double>(height) /
            1080.0,
            0.05,
            1.0);

    const double targetScale =
        std::pow(
            linearTargetScale,
            2.35);

    const double midpoint =
        0.03 +
        0.97 * targetScale;

    const double glowGain =
        static_cast<double>(
            std::clamp(glow_, 0, 100)) /
        100.0;

    /*
     * Keep the accepted mini-view width and the recently accepted thin
     * full-size beam.
     *
     * This is now interpreted as an approximate optical core width rather
     * than a Gaussian stamp sigma.
     */
    const double largeTargetBeamTaper =
        1.0 -
        0.6551724137931034 *
        midpoint * midpoint;

    const double legacyCoreSigma =
        0.18 +
        0.58 * midpoint *
        largeTargetBeamTaper;

    /*
     * Approximate half-width of the bright beam core.
     *
     * The lower clamp prevents tiny targets from becoming discontinuous,
     * while the full-size value stays close to the accepted ~half-width
     * look.  Antialias coverage extends another half pixel around this
     * geometric core.
     */
    /*
     * Small views may not fatten single traces unnecessarily.  Preserve the
     * accepted full-size beam, but taper the analytic core slightly on small
     * targets so mini views stay crisp without reintroducing gaps.
     */
    const double miniViewBlend =
        std::clamp(
            (renderDimension - 260.0) /
                640.0,
            0.0,
            1.0);

    const double miniViewThinFactor =
        0.74 +
        0.26 * miniViewBlend;

    const double coreHalfWidth =
        std::clamp(
            legacyCoreSigma * 0.78 * miniViewThinFactor,
            0.13,
            0.34);

    const double glowSigma =
        0.58 +
        0.92 * midpoint;

    /*
     * White core energy.  The old stamp path multiplied its kernel energy
     * by seven before accumulation.  This peak keeps the new core in the
     * same visual brightness ballpark without forcing overlapping segments
     * to become brighter at joins.
     */
    /*
     * Final Q-less energy -> display conversion uses energy >> 3.
     * 2040 therefore maps exactly to display white (255) without relying
     * on saturation.
     */
    constexpr std::uint16_t kCorePeakEnergy =
        2040u;

    std::array<GlowKernel,
        kSubpixelKernelCount> glowKernels{};

    /*
     * Glow only.  No white core lives in these kernels anymore.
     * Building the subpixel kernels contains hypot/exp work; measure it as a
     * separate phase so a non-zero glow cannot hide in pre-raster setup.
     */
    const std::uint64_t glowKernelStartUs =
        timelineBaseUs +
        static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
    QElapsedTimer glowKernelTimer;
    glowKernelTimer.start();

    if (glowGain > 0.0)
    {
        constexpr int kCoverageSamples = 4;
        constexpr double kCoverageInv =
            1.0 /
            static_cast<double>(
                kCoverageSamples *
                kCoverageSamples);

        for (int phaseY = 0;
            phaseY < kSubpixelPhases;
            ++phaseY)
        {
            const double fy =
                static_cast<double>(phaseY) /
                static_cast<double>(kSubpixelPhases);

            for (int phaseX = 0;
                phaseX < kSubpixelPhases;
                ++phaseX)
            {
                const double fx =
                    static_cast<double>(phaseX) /
                    static_cast<double>(kSubpixelPhases);

                GlowKernel& kernel =
                    glowKernels[
                        static_cast<std::size_t>(
                            phaseY * kSubpixelPhases +
                            phaseX)];

                for (int ky = -kGlowKernelRadius;
                    ky <= kGlowKernelRadius;
                    ++ky)
                {
                    for (int kx = -kGlowKernelRadius;
                        kx <= kGlowKernelRadius;
                        ++kx)
                    {
                        double accumulatedGlow = 0.0;

                        for (int sy = 0;
                            sy < kCoverageSamples;
                            ++sy)
                        {
                            const double py =
                                static_cast<double>(ky) -
                                0.5 +
                                (static_cast<double>(sy) + 0.5) /
                                static_cast<double>(kCoverageSamples);

                            for (int sx = 0;
                                sx < kCoverageSamples;
                                ++sx)
                            {
                                const double px =
                                    static_cast<double>(kx) -
                                    0.5 +
                                    (static_cast<double>(sx) + 0.5) /
                                    static_cast<double>(kCoverageSamples);

                                const double dx =
                                    px - fx;

                                const double dy =
                                    py - fy;

                                const double distance =
                                    std::hypot(
                                        dx,
                                        dy);

                                const double shoulder =
                                    std::clamp(
                                        (distance - midpoint) /
                                        std::max(
                                            0.18,
                                            1.70 - midpoint),
                                        0.0,
                                        1.0);

                                accumulatedGlow +=
                                    std::exp(
                                        -0.5 *
                                        (distance * distance) /
                                        (glowSigma * glowSigma)) *
                                    shoulder;
                            }
                        }

                        const double glowCoverage =
                            accumulatedGlow *
                            kCoverageInv;

                        const double glowEnergy =
                            240.0 *
                            glowGain *
                            glowCoverage;

                        kernel[
                            static_cast<std::size_t>(
                                (ky + kGlowKernelRadius) *
                                    kGlowKernelSize +
                                (kx + kGlowKernelRadius))] =
                            static_cast<std::uint16_t>(
                                std::clamp(
                                    static_cast<int>(
                                        std::lround(
                                            glowEnergy)),
                                    0,
                                    1023));
                    }
                }
            }
        }
    }

    const std::uint64_t glowKernelUs =
        static_cast<std::uint64_t>(glowKernelTimer.nsecsElapsed() / 1000);
    recordLocalPhase('H', glowKernelStartUs, glowKernelUs);

    const std::uint64_t aaSetupStartUs =
        timelineBaseUs +
        static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
    QElapsedTimer aaSetupTimer;
    aaSetupTimer.start();

    const auto addEnergy =
        [&currentEnergy,
         width,
         height](
            int x,
            int y,
            int contribution)
        {
            if (x < 0 ||
                x >= width ||
                y < 0 ||
                y >= height ||
                contribution <= 0)
            {
                return;
            }

            const std::size_t index =
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);

            const std::uint32_t sum =
                static_cast<std::uint32_t>(
                    currentEnergy[index]) +
                static_cast<std::uint32_t>(
                    contribution);

            currentEnergy[index] =
                static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(
                        sum,
                        65535u));
        };

    const auto maxCoreEnergy =
        [&currentEnergy,
         width,
         height](
            int x,
            int y,
            std::uint16_t contribution)
        {
            if (x < 0 ||
                x >= width ||
                y < 0 ||
                y >= height ||
                contribution == 0u)
            {
                return;
            }

            const std::size_t index =
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);

            currentEnergy[index] =
                std::max(
                    currentEnergy[index],
                    contribution);
        };

    constexpr int kCoreCoverageSamples = 2;
    constexpr double kCoreCoverageInv =
        1.0 /
        static_cast<double>(
            kCoreCoverageSamples *
            kCoreCoverageSamples);

    QElapsedTimer timer;
    timer.start();

    activeMinX = width;
    activeMinY = height;
    activeMaxX = -1;
    activeMaxY = -1;

    /*
     * Analytic white core.
     *
     * Pixel coordinates in this renderer are centred on integer target
     * coordinates.  For every Catweazle segment we visit only the small
     * bounding rectangle around that segment and evaluate the exact shortest
     * distance from the pixel centre to the segment.
     *
     * Coverage:
     *   <= halfWidth - 0.5  : fully covered
     *   >= halfWidth + 0.5  : not covered
     *   between             : linear subpixel edge coverage
     *
     * This is orientation-independent and cannot leave gaps between
     * consecutive connected segments.
     */
    const double aaRadius =
        coreHalfWidth +
        0.5;

    /*
     * AA SWITCH + STITCHER
     * --------------------
     * denseSteepSegment == true means the MUD sieve approved the segment
     * for the hard/no-expensive-AA path.  At every RED<->WHITE transition
     * keep two Catmull segments on both sides on FULL CatWuzle AA.  This
     * overlap is the stitcher: it prevents a hard cap/brightness discontinuity
     * from turning the packet boundary into a pair of searchlights.
     */
    const std::size_t segmentCount =
        polyline.size() - 1u;
    const bool antiAliasingEnabled =
        antiAliasing_.load(std::memory_order_acquire);

    std::vector<bool> segmentUsesFullAa(
        segmentCount,
        antiAliasingEnabled);

    if (antiAliasingEnabled &&
        denseSteepSegment.size() == segmentCount)
    {
        for (std::size_t segment = 0u; segment < segmentCount; ++segment)
        {
            segmentUsesFullAa[segment] = !denseSteepSegment[segment];
        }

        constexpr std::size_t kStitchSegments = 2u;
        for (std::size_t boundary = 1u; boundary < segmentCount; ++boundary)
        {
            if (denseSteepSegment[boundary - 1u] ==
                denseSteepSegment[boundary])
            {
                continue;
            }

            const std::size_t first =
                boundary > kStitchSegments
                    ? boundary - kStitchSegments
                    : 0u;
            const std::size_t last =
                std::min(
                    segmentCount - 1u,
                    boundary + kStitchSegments - 1u);

            for (std::size_t segment = first; segment <= last; ++segment)
            {
                segmentUsesFullAa[segment] = true;
            }
        }
    }

    /*
     * COST-BALANCED CATWUZLE X-STRIPS
     * --------------------------------
     * Every chunk owns a disjoint target-X range. All Catmull segments that
     * can affect that strip are evaluated there, so workers never write the
     * same pixel. This keeps the accepted max/add semantics deterministic
     * without private full-frame buffers or a zipper merge.
     */
    constexpr std::size_t kPreferredChunkCount = 8u;

    const std::uint64_t aaSetupUs =
        static_cast<std::uint64_t>(aaSetupTimer.nsecsElapsed() / 1000);
    recordLocalPhase('A', aaSetupStartUs, aaSetupUs);

    const std::uint64_t rasterLoadStartUs =
        timelineBaseUs +
        static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
    QElapsedTimer rasterLoadTimer;
    rasterLoadTimer.start();

    std::vector<std::uint64_t> columnCost(
        static_cast<std::size_t>(width),
        1u);

    activeMinX = width;
    activeMinY = height;
    activeMaxX = -1;
    activeMaxY = -1;

    for (std::size_t segment = 0u; segment < segmentCount; ++segment)
    {
        const BeamPoint& first = polyline[segment];
        const BeamPoint& second = polyline[segment + 1u];
        const double dx = second.x - first.x;
        const double dy = second.y - first.y;

        if (dx * dx + dy * dy <= 1.0e-18)
        {
            continue;
        }

        int firstX = 0;
        int lastX = -1;
        int firstY = 0;
        int lastY = -1;
        std::uint64_t perColumnCost = 1u;

        if (segmentUsesFullAa[segment])
        {
            firstX = std::max(0, static_cast<int>(
                std::floor(std::min(first.x, second.x) - aaRadius)));
            lastX = std::min(width - 1, static_cast<int>(
                std::ceil(std::max(first.x, second.x) + aaRadius)));
            firstY = std::max(0, static_cast<int>(
                std::floor(std::min(first.y, second.y) - aaRadius)));
            lastY = std::min(height - 1, static_cast<int>(
                std::ceil(std::max(first.y, second.y) + aaRadius)));

            const std::uint64_t rows =
                lastY >= firstY
                    ? static_cast<std::uint64_t>(lastY - firstY + 1)
                    : 1u;
            perColumnCost = 4u * rows + 4u;
        }
        else
        {
            const double longestAxis = std::max(std::abs(dx), std::abs(dy));
            const int steps = std::max(1, static_cast<int>(std::ceil(longestAxis)));
            firstX = std::max(0, static_cast<int>(
                std::floor(std::min(first.x, second.x))));
            lastX = std::min(width - 1, static_cast<int>(
                std::ceil(std::max(first.x, second.x))));
            firstY = std::max(0, static_cast<int>(
                std::floor(std::min(first.y, second.y))));
            lastY = std::min(height - 1, static_cast<int>(
                std::ceil(std::max(first.y, second.y))));

            const int span = std::max(1, lastX - firstX + 1);
            perColumnCost = std::max<std::uint64_t>(
                1u,
                (static_cast<std::uint64_t>(steps) + 1u) /
                    static_cast<std::uint64_t>(span));
        }

        if (lastX < firstX || lastY < firstY)
        {
            continue;
        }

        activeMinX = std::min(activeMinX, firstX);
        activeMaxX = std::max(activeMaxX, lastX);
        activeMinY = std::min(activeMinY, firstY);
        activeMaxY = std::max(activeMaxY, lastY);

        for (int x = firstX; x <= lastX; ++x)
        {
            columnCost[static_cast<std::size_t>(x)] += perColumnCost;
        }
    }

    const std::uint64_t rasterLoadUs =
        static_cast<std::uint64_t>(rasterLoadTimer.nsecsElapsed() / 1000);
    recordLocalPhase('L', rasterLoadStartUs, rasterLoadUs);

    const std::uint64_t jobPartitionStartUs =
        timelineBaseUs +
        static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);
    QElapsedTimer jobPartitionTimer;
    jobPartitionTimer.start();

    std::size_t chunkCount = 1u;
    if (traceJobExecutor_ &&
        width >= 320)
    {
        /*
         * Always create the R work queue when the executor exists.
         *
         * Helper availability is intentionally NOT sampled here.  W1/W2 may
         * still be busy with N/D/C/S when raster starts, but they can become
         * available while W0 is processing the same R generation.  If we
         * collapse to one strip at raster start, there is no remaining queue
         * for them to join later.
         */
        chunkCount = std::min<std::size_t>(
            kPreferredChunkCount,
            static_cast<std::size_t>(width));
    }

    struct CoreStrip
    {
        int firstX = 0;
        int lastX = -1;
    };

    std::vector<CoreStrip> strips;
    strips.reserve(chunkCount);

    if (chunkCount == 1u)
    {
        strips.push_back({0, width - 1});
    }
    else
    {
        const std::uint64_t totalCost =
            std::accumulate(columnCost.begin(), columnCost.end(), std::uint64_t{0});
        std::uint64_t accumulated = 0u;
        int stripFirstX = 0;

        for (std::size_t strip = 0u; strip + 1u < chunkCount; ++strip)
        {
            const std::uint64_t target =
                (totalCost * static_cast<std::uint64_t>(strip + 1u)) /
                static_cast<std::uint64_t>(chunkCount);

            int boundary = stripFirstX;
            while (boundary < width - 1 && accumulated < target)
            {
                accumulated += columnCost[static_cast<std::size_t>(boundary)];
                ++boundary;
            }

            const int remainingStrips =
                static_cast<int>(chunkCount - strip - 1u);
            boundary = std::clamp(
                boundary,
                stripFirstX + 1,
                width - remainingStrips);

            strips.push_back({stripFirstX, boundary - 1});
            stripFirstX = boundary;
        }

        strips.push_back({stripFirstX, width - 1});
    }

    frameStats.chunkCount = static_cast<std::uint32_t>(strips.size());
    renderTimings_.traceParallel = strips.size() > 1u;

    const std::uint64_t jobPartitionUs =
        static_cast<std::uint64_t>(jobPartitionTimer.nsecsElapsed() / 1000);
    recordLocalPhase('J', jobPartitionStartUs, jobPartitionUs);

    const auto renderCoreStrip =
        [&](int clipFirstX, int clipLastX)
        {
        for (std::size_t i = 1u;
            i < polyline.size();
            ++i)
        {
            const BeamPoint& first =
                polyline[i - 1u];

            const BeamPoint& second =
                polyline[i];

            const double segmentDx =
                second.x -
                first.x;

            const double segmentDy =
                second.y -
                first.y;

            const double segmentLengthSquared =
                segmentDx * segmentDx +
                segmentDy * segmentDy;

            if (segmentLengthSquared <= 1.0e-18)
            {
                continue;
            }

            const bool useFullAa =
                segmentUsesFullAa[i - 1u];

            if (!useFullAa)
            {
                /*
                 * MUD dense packet: NO AA AT ALL.
                 *
                 * Keep the exact fitted CatWuzle/Catmull polyline geometry, but
                 * rasterise this segment directly as a plain one-pixel DDA trace.
                 * No 2x2 coverage, no crossing guard, no coverage shaping and no
                 * round-join AA.  This is deliberately the same raw-curve idea as
                 * the accepted W0 no-AA reference, only selected by the detector.
                 */
                const double longestAxis =
                    std::max(std::abs(segmentDx), std::abs(segmentDy));

                if (longestAxis <= 1.0e-12)
                {
                    continue;
                }

                const std::uint16_t rawCoreEnergy =
                    static_cast<std::uint16_t>(
                        std::clamp(
                            static_cast<int>(
                                std::lround(
                                    static_cast<double>(kCorePeakEnergy) *
                                    static_cast<double>(coreIntensity_) /
                                    100.0)),
                            0,
                            static_cast<int>(kCorePeakEnergy) * 4));

                const int steps =
                    std::max(1, static_cast<int>(std::ceil(longestAxis)));

                for (int step = 0; step <= steps; ++step)
                {
                    const double t =
                        static_cast<double>(step) /
                        static_cast<double>(steps);

                    const int x =
                        static_cast<int>(
                            std::lround(first.x + segmentDx * t));
                    const int y =
                        static_cast<int>(
                            std::lround(first.y + segmentDy * t));

                    if (x < 0 || x >= width || y < 0 || y >= height)
                    {
                        continue;
                    }

                    if (x < clipFirstX || x > clipLastX)
                    {
                        continue;
                    }

                    maxCoreEnergy(x, y, rawCoreEnergy);
                }

                continue;
            }

            const int minX =
                std::max(
                    0,
                    static_cast<int>(
                        std::floor(
                            std::min(first.x, second.x) -
                            aaRadius)));

            const int maxX =
                std::min(
                    width - 1,
                    static_cast<int>(
                        std::ceil(
                            std::max(first.x, second.x) +
                            aaRadius)));

            const int minY =
                std::max(
                    0,
                    static_cast<int>(
                        std::floor(
                            std::min(first.y, second.y) -
                            aaRadius)));

            const int maxY =
                std::min(
                    height - 1,
                    static_cast<int>(
                        std::ceil(
                            std::max(first.y, second.y) +
                            aaRadius)));

            const int clippedMinX =
                std::max(minX, clipFirstX);
            const int clippedMaxX =
                std::min(maxX, clipLastX);

            if (clippedMaxX < clippedMinX)
            {
                continue;
            }

            for (int y = minY;
                y <= maxY;
                ++y)
            {
                const double py =
                    static_cast<double>(y);

                for (int x = clippedMinX;
                    x <= clippedMaxX;
                    ++x)
                {
                    const double px =
                        static_cast<double>(x);

                    double coverage = 0.0;

                    {
                        /*
                         * Accepted CatWuzle 2x2 analytic area coverage.
                         */
                        const double maximumCoverage =
                            coreHalfWidth + 0.5;

                        for (int sampleY = 0;
                            sampleY < kCoreCoverageSamples;
                            ++sampleY)
                        {
                            const double subpixelY =
                                py - 0.5 +
                                (static_cast<double>(sampleY) + 0.5) /
                                    static_cast<double>(kCoreCoverageSamples);

                            for (int sampleX = 0;
                                sampleX < kCoreCoverageSamples;
                                ++sampleX)
                            {
                                const double subpixelX =
                                    px - 0.5 +
                                    (static_cast<double>(sampleX) + 0.5) /
                                        static_cast<double>(kCoreCoverageSamples);

                                const double pointDx = subpixelX - first.x;
                                const double pointDy = subpixelY - first.y;
                                const double projection = std::clamp(
                                    (pointDx * segmentDx + pointDy * segmentDy) /
                                        segmentLengthSquared,
                                    0.0,
                                    1.0);
                                const double closestX = first.x + projection * segmentDx;
                                const double closestY = first.y + projection * segmentDy;
                                const double dx = subpixelX - closestX;
                                const double dy = subpixelY - closestY;
                                const double distance = std::hypot(dx, dy);

                                coverage += std::clamp(
                                    (maximumCoverage - distance) / maximumCoverage,
                                    0.0,
                                    1.0);
                            }
                        }

                        coverage *= kCoreCoverageInv;

                        /* Pixel-square crossing guard: unchanged on FULL AA. */
                        {
                            const double pixelMinX = px - 0.5;
                            const double pixelMaxX = px + 0.5;
                            const double pixelMinY = py - 0.5;
                            const double pixelMaxY = py + 0.5;
                            double enterT = 0.0;
                            double exitT = 1.0;

                            const auto clipAxis =
                                [&enterT, &exitT](double origin,
                                                 double delta,
                                                 double minimum,
                                                 double maximum) -> bool
                                {
                                    constexpr double kTiny = 1.0e-12;
                                    if (std::abs(delta) <= kTiny)
                                    {
                                        return origin >= minimum && origin <= maximum;
                                    }
                                    double firstT = (minimum - origin) / delta;
                                    double secondT = (maximum - origin) / delta;
                                    if (firstT > secondT)
                                    {
                                        std::swap(firstT, secondT);
                                    }
                                    enterT = std::max(enterT, firstT);
                                    exitT = std::min(exitT, secondT);
                                    return enterT <= exitT;
                                };

                            const bool crossesPixel =
                                clipAxis(first.x, segmentDx, pixelMinX, pixelMaxX) &&
                                clipAxis(first.y, segmentDy, pixelMinY, pixelMaxY) &&
                                exitT >= 0.0 && enterT <= 1.0;

                            if (crossesPixel)
                            {
                                constexpr double kMinimumCrossingCoverage = 0.20;
                                coverage = std::max(coverage, kMinimumCrossingCoverage);
                            }
                        }

                        const double coverageLift = 1.16 + 0.12 * miniViewBlend;
                        coverage = 1.0 - std::pow(1.0 - coverage, coverageLift);
                    }

                    if (coverage <= 0.0)
                    {
                        continue;
                    }

                    const std::uint16_t energy =
                        static_cast<std::uint16_t>(
                            std::clamp(
                                static_cast<int>(
                                    std::lround(
                                        coverage *
                                        static_cast<double>(
                                            kCorePeakEnergy) *
                                        static_cast<double>(
                                            coreIntensity_) /
                                        100.0)),
                                0,
                                static_cast<int>(
                                    kCorePeakEnergy) * 4));

                    maxCoreEnergy(
                        x,
                        y,
                        energy);
                }
            }
        }


        /*
         * CATWUZLE ROUND-JOIN COVERAGE
         * ----------------------------
         *
         * The 2x2 area coverage above is evaluated per segment and the final
         * pixel energy is combined with max().  At an interior polyline vertex,
         * subpixel samples can be split across the two neighbouring segments:
         * neither segment alone sees the complete covered area, even though the
         * union of both segments does.  That produces a stationary local notch
         * (the visible "bite") at some joins.
         *
         * Rasterise the mathematically correct round join at every interior
         * Catweazle vertex.  It uses exactly the same core radius, 2x2 area
         * coverage, shaping and intensity as the segment core, and combines via
         * max(), so it fills only missing join coverage without widening the
         * normal stroke.
         */
        for (std::size_t i = 1u;
            i + 1u < polyline.size();
            ++i)
        {
            // No round-join AA inside a dense hard-core packet.  The stitcher
            // makes at least one side FULL at packet boundaries, so transitions
            // still receive the accepted join treatment.
            if (i - 1u < segmentUsesFullAa.size() &&
                i < segmentUsesFullAa.size() &&
                !segmentUsesFullAa[i - 1u] &&
                !segmentUsesFullAa[i])
            {
                continue;
            }

            const BeamPoint& vertex =
                polyline[i];

            const int minX =
                std::max(
                    0,
                    static_cast<int>(
                        std::floor(
                            vertex.x -
                            aaRadius)));

            const int maxX =
                std::min(
                    width - 1,
                    static_cast<int>(
                        std::ceil(
                            vertex.x +
                            aaRadius)));

            const int minY =
                std::max(
                    0,
                    static_cast<int>(
                        std::floor(
                            vertex.y -
                            aaRadius)));

            const int maxY =
                std::min(
                    height - 1,
                    static_cast<int>(
                        std::ceil(
                            vertex.y +
                            aaRadius)));

            const int clippedMinX =
                std::max(minX, clipFirstX);
            const int clippedMaxX =
                std::min(maxX, clipLastX);

            if (clippedMaxX < clippedMinX)
            {
                continue;
            }

            for (int y = minY;
                y <= maxY;
                ++y)
            {
                const double py =
                    static_cast<double>(y);

                for (int x = clippedMinX;
                    x <= clippedMaxX;
                    ++x)
                {
                    const double px =
                        static_cast<double>(x);

                    const double maximumCoverage =
                        coreHalfWidth +
                        0.5;

                    double coverage = 0.0;

                    for (int sampleY = 0;
                        sampleY < kCoreCoverageSamples;
                        ++sampleY)
                    {
                        const double subpixelY =
                            py - 0.5 +
                            (static_cast<double>(sampleY) + 0.5) /
                                static_cast<double>(kCoreCoverageSamples);

                        for (int sampleX = 0;
                            sampleX < kCoreCoverageSamples;
                            ++sampleX)
                        {
                            const double subpixelX =
                                px - 0.5 +
                                (static_cast<double>(sampleX) + 0.5) /
                                    static_cast<double>(kCoreCoverageSamples);

                            const double dx =
                                subpixelX -
                                vertex.x;

                            const double dy =
                                subpixelY -
                                vertex.y;

                            const double distance =
                                std::hypot(
                                    dx,
                                    dy);

                            coverage +=
                                std::clamp(
                                    (maximumCoverage -
                                        distance) /
                                        maximumCoverage,
                                    0.0,
                                    1.0);
                        }
                    }

                    coverage *=
                        kCoreCoverageInv;

                    const double coverageLift =
                        1.16 +
                        0.12 * miniViewBlend;

                    coverage =
                        1.0 -
                        std::pow(
                            1.0 - coverage,
                            coverageLift);

                    if (coverage <= 0.0)
                    {
                        continue;
                    }

                    const std::uint16_t energy =
                        static_cast<std::uint16_t>(
                            std::clamp(
                                static_cast<int>(
                                    std::lround(
                                        coverage *
                                        static_cast<double>(
                                            kCorePeakEnergy) *
                                        static_cast<double>(
                                            coreIntensity_) /
                                        100.0)),
                                0,
                                static_cast<int>(
                                    kCorePeakEnergy) * 4));

                    maxCoreEnergy(
                        x,
                        y,
                        energy);
                }
            }
        }

        };

    std::vector<std::uint64_t> chunkRenderUs(strips.size(), 0u);
    std::vector<std::uint32_t> chunkWorker(strips.size(), 0u);

    const auto renderChunk =
        [&](std::size_t chunkIndex, std::uint32_t workerId)
        {
            if (chunkIndex >= strips.size())
            {
                return;
            }

            QElapsedTimer chunkTimer;
            chunkTimer.start();

            const CoreStrip& strip = strips[chunkIndex];
            renderCoreStrip(strip.firstX, strip.lastX);

            chunkRenderUs[chunkIndex] =
                static_cast<std::uint64_t>(chunkTimer.nsecsElapsed() / 1000);
            chunkWorker[chunkIndex] = workerId;
        };

    if (strips.size() > 1u && traceJobExecutor_)
    {
        traceJobExecutor_('R', strips.size(), renderChunk);
    }
    else
    {
        renderChunk(0u, 0u);
    }

    if (!chunkRenderUs.empty())
    {
        const auto [minimumIt, maximumIt] =
            std::minmax_element(chunkRenderUs.begin(), chunkRenderUs.end());
        const std::uint64_t totalChunkUs =
            std::accumulate(chunkRenderUs.begin(), chunkRenderUs.end(), std::uint64_t{0});

        frameStats.chunkRenderMinUs = *minimumIt;
        frameStats.chunkRenderMaxUs = *maximumIt;
        frameStats.chunkRenderAvgUs =
            totalChunkUs / static_cast<std::uint64_t>(chunkRenderUs.size());

        for (std::size_t chunkIndex = 0u; chunkIndex < chunkRenderUs.size(); ++chunkIndex)
        {
            const std::size_t workerIndex =
                std::min<std::size_t>(
                    static_cast<std::size_t>(chunkWorker[chunkIndex]),
                    frameStats.workerChunkCount.size() - 1u);
            ++frameStats.workerChunkCount[workerIndex];
            frameStats.workerRenderUs[workerIndex] += chunkRenderUs[chunkIndex];
        }
    }

    coreUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    /*
     * Cheap local glow pass -- Delta 63: spread the stamp/apply work over
     * the existing WF/V1/V2/VS assist queue.
     *
     * Each assist job owns a disjoint band of target rows.  All jobs scan the
     * same polyline, but a job only applies kernel rows that land inside its
     * own band.  That keeps currentEnergy writes race-free without atomics or
     * per-worker full-frame scratch buffers.  The executor is synchronous, so
     * completion of traceJobExecutor_ is the barrier before resolve/output X.
     */
    const std::uint64_t glowStampStartUs =
        timelineBaseUs +
        static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);

    timer.restart();

    bool usedParallelGlow = false;

    if (glowGain > 0.0 && polyline.size() > 1u)
    {
        double minimumBeamX = polyline.front().x;
        double maximumBeamX = polyline.front().x;
        double minimumBeamY = polyline.front().y;
        double maximumBeamY = polyline.front().y;

        for (const BeamPoint& point : polyline)
        {
            minimumBeamX = std::min(minimumBeamX, point.x);
            maximumBeamX = std::max(maximumBeamX, point.x);
            minimumBeamY = std::min(minimumBeamY, point.y);
            maximumBeamY = std::max(maximumBeamY, point.y);
        }

        // One extra pixel covers the phase-rounding carry that can move a
        // centre from floor(x/y) to the next pixel.
        const int glowMinX =
            static_cast<int>(std::floor(minimumBeamX)) -
            kGlowKernelRadius;
        const int glowMaxX =
            static_cast<int>(std::ceil(maximumBeamX)) +
            kGlowKernelRadius + 1;
        const int glowMinY =
            static_cast<int>(std::floor(minimumBeamY)) -
            kGlowKernelRadius;
        const int glowMaxY =
            static_cast<int>(std::ceil(maximumBeamY)) +
            kGlowKernelRadius + 1;

        activeMinX = std::min(activeMinX, glowMinX);
        activeMaxX = std::max(activeMaxX, glowMaxX);
        activeMinY = std::min(activeMinY, glowMinY);
        activeMaxY = std::max(activeMaxY, glowMaxY);

        const int firstTargetRow = std::max(0, glowMinY);
        const int lastTargetRow = std::min(height - 1, glowMaxY);
        const int targetRows =
            std::max(0, lastTargetRow - firstTargetRow + 1);

        constexpr int kTargetGlowRowsPerJob = 48;
        constexpr std::size_t kMaxGlowJobs = 8u;

        const std::size_t desiredGlowJobs =
            targetRows > 0
            ? static_cast<std::size_t>(
                std::max(
                    1,
                    (targetRows + kTargetGlowRowsPerJob - 1) /
                        kTargetGlowRowsPerJob))
            : 1u;

        const std::size_t glowJobCount =
            std::min(kMaxGlowJobs, desiredGlowJobs);

        struct GlowSample
        {
            int centerX = 0;
            int centerY = 0;
            std::uint16_t kernelIndex = 0u;
        };

        /*
         * Delta 67: prepare every glow sample exactly once, then bucket it
         * into only the output-row jobs whose 5x5 kernel can touch that band.
         *
         * Delta 63 already made the destination rows race-free, but every
         * worker still walked the complete polyline and recomputed every
         * segment step/phase before rejecting rows it did not own.  That
         * multiplied most of the CPU work by the worker count and explained
         * why four busy g workers gave almost no wallclock improvement.
         *
         * Bucket insertion preserves source-sample order per output band, so
         * energy accumulation order within every destination pixel remains
         * the same as in the row-owned Delta 63 path.
         */
        /*
         * Delta 68: parallelise the expensive sample preparation too.
         *
         * Each prep job owns a contiguous range of polyline segments and
         * builds private row-band buckets.  Private buckets avoid locks while
         * preserving source order: renderGlowJob visits prep jobs in segment
         * order, then the samples produced by each job in their original
         * segment/step order.
         *
         * This removes the Delta 67 serial polyline walk that merely moved the
         * old ~4 ms glow cost in front of the parallel g workers.
         */
        const std::size_t segmentCount = polyline.size() - 1u;
        const std::size_t glowPrepJobCount =
            std::min<std::size_t>(
                kMaxGlowJobs,
                std::max<std::size_t>(1u, segmentCount));

        using GlowBuckets = std::vector<std::vector<GlowSample>>;
        std::vector<GlowBuckets> prepBuckets(
            glowPrepJobCount,
            GlowBuckets(glowJobCount));

        const auto jobFirstY =
            [&](std::size_t jobIndex)
            {
                return firstTargetRow +
                    static_cast<int>(
                        (static_cast<std::int64_t>(jobIndex) *
                            static_cast<std::int64_t>(targetRows)) /
                        static_cast<std::int64_t>(glowJobCount));
            };

        const auto jobOnePastLastY =
            [&](std::size_t jobIndex)
            {
                return firstTargetRow +
                    static_cast<int>(
                        (static_cast<std::int64_t>(jobIndex + 1u) *
                            static_cast<std::int64_t>(targetRows)) /
                        static_cast<std::int64_t>(glowJobCount));
            };

        const auto rowToGlowJob =
            [&](int row)
            {
                const int clippedRow =
                    std::clamp(row, firstTargetRow, lastTargetRow);
                const std::int64_t offset =
                    static_cast<std::int64_t>(clippedRow - firstTargetRow);
                const std::int64_t numerator =
                    (offset + 1) * static_cast<std::int64_t>(glowJobCount) - 1;

                return std::min<std::size_t>(
                    glowJobCount - 1u,
                    static_cast<std::size_t>(
                        numerator / static_cast<std::int64_t>(targetRows)));
            };

        const std::uint64_t glowSetupEndUs =
            timelineBaseUs +
            static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);

        recordLocalPhase(
            's',
            glowStampStartUs,
            glowSetupEndUs > glowStampStartUs
                ? glowSetupEndUs - glowStampStartUs
                : 0u);

        const auto prepareGlowJob =
            [&](std::size_t prepJobIndex)
            {
                const std::size_t firstSegment =
                    (prepJobIndex * segmentCount) / glowPrepJobCount;
                const std::size_t onePastLastSegment =
                    ((prepJobIndex + 1u) * segmentCount) / glowPrepJobCount;

                GlowBuckets& buckets = prepBuckets[prepJobIndex];

                for (auto& bucket : buckets)
                {
                    bucket.reserve(
                        std::max<std::size_t>(
                            32u,
                            polyline.size() /
                                std::max<std::size_t>(
                                    1u,
                                    glowPrepJobCount * glowJobCount)));
                }

                for (std::size_t segmentIndex = firstSegment;
                    segmentIndex < onePastLastSegment;
                    ++segmentIndex)
                {
                    const BeamPoint& first = polyline[segmentIndex];
                    const BeamPoint& second = polyline[segmentIndex + 1u];

                    const double segmentDx = second.x - first.x;
                    const double segmentDy = second.y - first.y;
                    const double segmentLength =
                        std::hypot(segmentDx, segmentDy);

                    if (segmentLength <= 1.0e-9)
                    {
                        continue;
                    }

                    const int steps =
                        std::max(
                            1,
                            static_cast<int>(
                                std::ceil(segmentLength)));

                    // Preserve the original whole-polyline sampling rule:
                    // only the very first segment emits step zero.
                    const int firstStep =
                        segmentIndex == 0u ? 0 : 1;

                    for (int step = firstStep;
                        step <= steps;
                        ++step)
                    {
                        const double t =
                            static_cast<double>(step) /
                            static_cast<double>(steps);

                        const double beamX = first.x + segmentDx * t;
                        const double beamY = first.y + segmentDy * t;

                        int centerX =
                            static_cast<int>(std::floor(beamX));
                        int centerY =
                            static_cast<int>(std::floor(beamY));

                        const double fracX =
                            beamX - static_cast<double>(centerX);
                        const double fracY =
                            beamY - static_cast<double>(centerY);

                        int phaseX =
                            static_cast<int>(
                                std::lround(
                                    fracX *
                                    static_cast<double>(kSubpixelPhases)));
                        int phaseY =
                            static_cast<int>(
                                std::lround(
                                    fracY *
                                    static_cast<double>(kSubpixelPhases)));

                        if (phaseX >= kSubpixelPhases)
                        {
                            phaseX = 0;
                            ++centerX;
                        }

                        if (phaseY >= kSubpixelPhases)
                        {
                            phaseY = 0;
                            ++centerY;
                        }

                        const GlowSample sample{
                            centerX,
                            centerY,
                            static_cast<std::uint16_t>(
                                phaseY * kSubpixelPhases + phaseX)
                        };

                        const int sampleFirstY =
                            centerY - kGlowKernelRadius;
                        const int sampleLastY =
                            centerY + kGlowKernelRadius;

                        if (sampleLastY < firstTargetRow ||
                            sampleFirstY > lastTargetRow)
                        {
                            continue;
                        }

                        const std::size_t firstGlowJob =
                            rowToGlowJob(
                                std::max(sampleFirstY, firstTargetRow));
                        const std::size_t lastGlowJob =
                            rowToGlowJob(
                                std::min(sampleLastY, lastTargetRow));

                        for (std::size_t glowJobIndex = firstGlowJob;
                            glowJobIndex <= lastGlowJob;
                            ++glowJobIndex)
                        {
                            buckets[glowJobIndex].push_back(sample);
                        }
                    }
                }
            };

        const std::uint64_t glowPrepStartUs =
            timelineBaseUs +
            static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);

        if (glowPrepJobCount > 1u && traceJobExecutor_)
        {
            traceJobExecutor_(
                'h',
                glowPrepJobCount,
                [&](std::size_t prepJobIndex, std::uint32_t /* workerId */)
                {
                    prepareGlowJob(prepJobIndex);
                });
        }
        else
        {
            prepareGlowJob(0u);
        }

        const std::uint64_t glowPrepEndUs =
            timelineBaseUs +
            static_cast<std::uint64_t>(phaseClock.nsecsElapsed() / 1000);

        // Whole-pass wallclock marker.  The per-worker h chunks (when
        // present) remain in the assist timelines; this aggregate marker
        // guarantees that the interval itself can never disappear from the
        // authoritative waveform chronology.
        recordLocalPhase(
            'h',
            glowPrepStartUs,
            glowPrepEndUs > glowPrepStartUs
                ? glowPrepEndUs - glowPrepStartUs
                : 0u);

        const auto renderGlowJob =
            [&](std::size_t jobIndex)
            {
                const int ownedFirstY = jobFirstY(jobIndex);
                const int onePastOwnedLastY = jobOnePastLastY(jobIndex);

                if (onePastOwnedLastY <= ownedFirstY)
                {
                    return;
                }

                const int ownedLastY = onePastOwnedLastY - 1;

                for (std::size_t prepJobIndex = 0u;
                    prepJobIndex < prepBuckets.size();
                    ++prepJobIndex)
                {
                    const auto& samples =
                        prepBuckets[prepJobIndex][jobIndex];

                    for (const GlowSample& sample : samples)
                    {
                        const int kernelFirstY =
                        std::max(
                            -kGlowKernelRadius,
                            ownedFirstY - sample.centerY);
                        const int kernelLastY =
                            std::min(
                            kGlowKernelRadius,
                            ownedLastY - sample.centerY);

                    const GlowKernel& glowKernel =
                        glowKernels[
                            static_cast<std::size_t>(sample.kernelIndex)];

                    for (int ky = kernelFirstY;
                        ky <= kernelLastY;
                        ++ky)
                    {
                        const std::size_t kernelRow =
                            static_cast<std::size_t>(
                                (ky + kGlowKernelRadius) *
                                kGlowKernelSize);

                        for (int kx = -kGlowKernelRadius;
                            kx <= kGlowKernelRadius;
                            ++kx)
                        {
                            const int value =
                                glowKernel[
                                    kernelRow +
                                    static_cast<std::size_t>(
                                        kx + kGlowKernelRadius)];

                            if (value <= 0)
                            {
                                continue;
                            }

                            addEnergy(
                                sample.centerX + kx,
                                sample.centerY + ky,
                                value * 7);
                        }
                    }
                }
            }
            };

        if (targetRows > 0 &&
            glowJobCount > 1u &&
            traceJobExecutor_)
        {
            usedParallelGlow = true;

            traceJobExecutor_(
                'g',
                glowJobCount,
                [&](std::size_t jobIndex, std::uint32_t /* workerId */)
                {
                    renderGlowJob(jobIndex);
                });
        }
        else if (targetRows > 0)
        {
            renderGlowJob(0u);
        }
    }

    glowUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    // Parallel g chunks are already emitted by the shared assist executor.
    // Keep a local whole-pass g event only on the serial fallback path.
    if (glowGain > 0.0 && !usedParallelGlow)
    {
        recordLocalPhase(
            'g',
            glowStampStartUs,
            glowUs);
    }

    return;
}

void WaveformRenderer::clearScopephorFrames()
{
    scopephorPreviousEnergy_.clear();
    scopephorPreviousMinX_ = 0;
    scopephorPreviousMinY_ = 0;
    scopephorPreviousMaxX_ = -1;
    scopephorPreviousMaxY_ = -1;
}

void WaveformRenderer::applyScopephorFeedback(
    std::vector<std::uint16_t>& currentEnergy,
    int& activeMinX,
    int& activeMinY,
    int& activeMaxX,
    int& activeMaxY)
{
    if (currentEnergy.empty())
    {
        return;
    }

    const int width = image_.width();
    const int height = image_.height();

    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (persistence_ <= 0)
    {
        scopephorPreviousEnergy_.clear();
        scopephorPreviousMinX_ = 0;
        scopephorPreviousMinY_ = 0;
        scopephorPreviousMaxX_ = -1;
        scopephorPreviousMaxY_ = -1;
        return;
    }

    const double slider =
        static_cast<double>(
            std::clamp(persistence_, 0, 100)) /
        100.0;

    /*
     * ScopePhor should now hang a bit longer.
     *
     * Keep the same general curve shape, but raise the retention factor
     * modestly so low/mid settings remain useful while the default setting
     * clearly glows longer on screen.
     */
    const double decay =
        0.72 *
        std::pow(slider, 0.36);

    if (scopephorPreviousEnergy_.size() !=
        currentEnergy.size())
    {
        scopephorPreviousEnergy_.assign(
            currentEnergy.size(),
            0u);

        scopephorPreviousMinX_ = 0;
        scopephorPreviousMinY_ = 0;
        scopephorPreviousMaxX_ = -1;
        scopephorPreviousMaxY_ = -1;
    }

    int unionMinX = activeMinX;
    int unionMinY = activeMinY;
    int unionMaxX = activeMaxX;
    int unionMaxY = activeMaxY;

    const bool previousValid =
        scopephorPreviousMaxX_ >= scopephorPreviousMinX_ &&
        scopephorPreviousMaxY_ >= scopephorPreviousMinY_;

    const bool currentValid =
        unionMaxX >= unionMinX &&
        unionMaxY >= unionMinY;

    if (previousValid)
    {
        if (!currentValid)
        {
            unionMinX = scopephorPreviousMinX_;
            unionMinY = scopephorPreviousMinY_;
            unionMaxX = scopephorPreviousMaxX_;
            unionMaxY = scopephorPreviousMaxY_;
        }
        else
        {
            unionMinX = std::min(unionMinX, scopephorPreviousMinX_);
            unionMinY = std::min(unionMinY, scopephorPreviousMinY_);
            unionMaxX = std::max(unionMaxX, scopephorPreviousMaxX_);
            unionMaxY = std::max(unionMaxY, scopephorPreviousMaxY_);
        }
    }

    if (unionMaxX < unionMinX ||
        unionMaxY < unionMinY)
    {
        return;
    }

    unionMinX = std::clamp(unionMinX, 0, width - 1);
    unionMaxX = std::clamp(unionMaxX, 0, width - 1);
    unionMinY = std::clamp(unionMinY, 0, height - 1);
    unionMaxY = std::clamp(unionMaxY, 0, height - 1);

    const std::uint32_t decayQ16 =
        static_cast<std::uint32_t>(
            std::clamp(
                static_cast<int>(
                    std::lround(decay * 65536.0)),
                0,
                65536));

    int nextMinX = width;
    int nextMinY = height;
    int nextMaxX = -1;
    int nextMaxY = -1;

    const __m256i decayVector =
        _mm256_set1_epi32(
            static_cast<int>(
                decayQ16));

    const __m256i thresholdVector =
        _mm256_set1_epi32(7);

    const __m256i maxEnergyVector =
        _mm256_set1_epi32(65535);

    for (int y = unionMinY;
        y <= unionMaxY;
        ++y)
    {
        const std::size_t row =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width);

        int x = unionMinX;
        bool rowHasEnergy = false;

        for (;
            x + 7 <= unionMaxX;
            x += 8)
        {
            const std::size_t index =
                row +
                static_cast<std::size_t>(x);

            const __m128i old16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        scopephorPreviousEnergy_.data() +
                        index));

            const __m128i current16 =
                _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(
                        currentEnergy.data() +
                        index));

            const __m256i old32 =
                _mm256_cvtepu16_epi32(old16);

            const __m256i current32 =
                _mm256_cvtepu16_epi32(current16);

            const __m256i oldScaled =
                _mm256_srli_epi32(
                    _mm256_mullo_epi32(
                        old32,
                        decayVector),
                    16);

            __m256i mixed =
                _mm256_add_epi32(
                    current32,
                    oldScaled);

            mixed =
                _mm256_min_epi32(
                    mixed,
                    maxEnergyVector);

            // Kill sub-visible residual phosphor.
            const __m256i visibleMask =
                _mm256_cmpgt_epi32(
                    mixed,
                    thresholdVector);

            mixed =
                _mm256_and_si256(
                    mixed,
                    visibleMask);

            const __m128i mixedLow =
                _mm256_castsi256_si128(
                    mixed);

            const __m128i mixedHigh =
                _mm256_extracti128_si256(
                    mixed,
                    1);

            const __m128i packed =
                _mm_packus_epi32(
                    mixedLow,
                    mixedHigh);

            _mm_storeu_si128(
                reinterpret_cast<__m128i*>(
                    currentEnergy.data() +
                    index),
                packed);

            _mm_storeu_si128(
                reinterpret_cast<__m128i*>(
                    scopephorPreviousEnergy_.data() +
                    index),
                packed);

            if (_mm256_movemask_epi8(
                    visibleMask) != 0)
            {
                rowHasEnergy = true;
                nextMinX =
                    std::min(
                        nextMinX,
                        x);

                nextMaxX =
                    std::max(
                        nextMaxX,
                        x + 7);
            }
        }

        for (;
            x <= unionMaxX;
            ++x)
        {
            const std::size_t index =
                row +
                static_cast<std::size_t>(x);

            const std::uint32_t oldContribution =
                (static_cast<std::uint32_t>(
                    scopephorPreviousEnergy_[index]) *
                 decayQ16) >>
                16;

            const std::uint32_t mixed =
                static_cast<std::uint32_t>(
                    currentEnergy[index]) +
                oldContribution;

            std::uint16_t value =
                static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(
                        mixed,
                        65535u));

            if (value < 8u)
            {
                value = 0u;
            }

            currentEnergy[index] = value;
            scopephorPreviousEnergy_[index] = value;

            if (value != 0u)
            {
                rowHasEnergy = true;
                nextMinX =
                    std::min(
                        nextMinX,
                        x);

                nextMaxX =
                    std::max(
                        nextMaxX,
                        x);
            }
        }

        if (rowHasEnergy)
        {
            nextMinY =
                std::min(
                    nextMinY,
                    y);

            nextMaxY =
                std::max(
                    nextMaxY,
                    y);
        }
    }

    scopephorPreviousMinX_ = nextMinX;
    scopephorPreviousMinY_ = nextMinY;
    scopephorPreviousMaxX_ = nextMaxX;
    scopephorPreviousMaxY_ = nextMaxY;

    activeMinX = nextMinX;
    activeMinY = nextMinY;
    activeMaxX = nextMaxX;
    activeMaxY = nextMaxY;
}

void WaveformRenderer::drawLineInfoOverlay(QPainter& painter)
{
    if (!lineInfoOverlayEnabled_ ||
        !lineInfoOverlayPalOutput_)
    {
        // The interactive PC LINE / X card is drawn by WaveformWidget in
        // widget coordinates next to the gear.  Keep this renderer overlay
        // only for PAL/Spout output, where there is no interactive gear.
        return;
    }

    const QRectF viewport =
        viewportRect();

    const QRectF scope =
        scaledScopeRect();

    const QVector<ViewportOverlay::InfoRow> rows
    {
        {
            QStringLiteral("LINE"),
            QStringLiteral("%1   X%2")
                .arg(
                    selectedLine_ >= 0
                    ? QString::number(selectedLine_)
                    : QStringLiteral("ALL"))
                .arg(zoomFactor_)
        }
    };

    const double referenceHeight =
        lineInfoOverlayPalOutput_
        ? viewport.height()
        : std::min(viewport.height(), 720.0);

    const QSizeF cardSize =
        ViewportOverlay::infoCardRequiredSize(
            rows,
            referenceHeight,
            lineInfoOverlayPalOutput_);

    const double lineGap =
        std::max(
            4.0,
            viewport.height() * 0.008);

    // Keep the line/zoom readout at the top-right.  The top-left is
    // intentionally left free for the automatic SNR readout.
    // On the PC waveform the top-right corner now contains the shortcuts
    // gear.  Reserve room for it; PAL/Spout has no interactive gear.
    const QRectF cardRect(
        scope.right() - cardSize.width(),
        scope.top() + lineGap,
        cardSize.width(),
        cardSize.height());

    ViewportOverlay::drawInfoCard(
        painter,
        cardRect,
        rows,
        referenceHeight,
        lineInfoOverlayPalOutput_);
}


void WaveformRenderer::renderAllLines(
    const Yuv444Frame& frame)
{
    renderTimings_ = {};

    QElapsedTimer phaseTimer;
    phaseTimer.start();

    sourceY_.clear();

    std::fill(
        hits_.begin(),
        hits_.end(),
        0u);

    renderTimings_.persistenceUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();

    const int displayWidth =
        image_.width();

    const int displayHeight =
        image_.height();

    const QRectF scope =
        scaledScopeRect();

    const double plotRenderDimension =
        std::max(
            1.0,
            std::min(
                scope.width(),
                scope.height()));

    const double plotBeamScale =
        std::clamp(
            std::sqrt(
                plotRenderDimension / 576.0),
            1.0,
            1.90);

    constexpr double kReferenceCoreRadiusPx = 0.82;
    beamCoreRadiusPx_ =
        kReferenceCoreRadiusPx * plotBeamScale;

    renderTimings_.beamCoreRadiusPx =
        beamCoreRadiusPx_;
    renderTimings_.beamCoreMarginPx =
        std::max(
            1,
            static_cast<int>(
                std::floor(beamCoreRadiusPx_)));

    const int firstScopeX =
        std::clamp(
            static_cast<int>(
                std::ceil(scope.left())),
            0,
            displayWidth - 1);

    const int lastScopeX =
        std::clamp(
            static_cast<int>(
                std::floor(scope.right())),
            0,
            displayWidth - 1);

    const auto digitalLevels =
        levels(VideoStandard::pal625());

    const auto analog =
        analogLevels(
            VideoColorStandard::Rec601_625);

    const double voltsPerCode =
        (analog.whiteVolts - analog.blackVolts) /
        static_cast<double>(
            digitalLevels.yWhite -
            digitalLevels.yBlack);

    const double voltsToPixels =
        scope.height() /
        analog.graticuleMaxVolts;

    for (int line = 0;
        line < frame.height;
        ++line)
    {
        const std::size_t lineOffset =
            static_cast<std::size_t>(line) *
            static_cast<std::size_t>(
                frame.width);

        int previousPlotY = -1;

        for (int sourceX = 0;
            sourceX < frame.width;
            ++sourceX)
        {
            const double normalisedX =
                frame.width > 1
                ? static_cast<double>(sourceX) /
                static_cast<double>(
                    frame.width - 1)
                : 0.0;

            const int displayX =
                std::clamp(
                    static_cast<int>(
                        std::lround(
                            scope.left() +
                            normalisedX *
                            scope.width())),
                    firstScopeX,
                    lastScopeX);

            const std::uint16_t y16 =
                frame.y[
                    lineOffset +
                        static_cast<std::size_t>(
                            sourceX)];

            const double y10 =
                static_cast<double>(y16) /
                64.0;

            const double volts =
                analog.blackVolts +
                (y10 - digitalLevels.yBlack) *
                voltsPerCode;

            const double plotYFloat =
                scope.bottom() -
                volts *
                voltsToPixels;

            const int plotY =
                std::clamp(
                    static_cast<int>(
                        std::lround(
                            plotYFloat)),
                    0,
                    displayHeight - 1);

            const double fractionalY =
                plotYFloat -
                std::floor(plotYFloat);

            const std::uint32_t fraction =
                static_cast<std::uint32_t>(
                    std::clamp(
                        fractionalY * 256.0,
                        0.0,
                        255.0));

            const std::size_t currentIndex =
                static_cast<std::size_t>(plotY) *
                static_cast<std::size_t>(
                    displayWidth) +
                static_cast<std::size_t>(
                    displayX);

            hits_[currentIndex] +=
                256u - fraction;

            if (plotY > 0)
            {
                const std::size_t adjacentIndex =
                    static_cast<std::size_t>(
                        plotY - 1) *
                    static_cast<std::size_t>(
                        displayWidth) +
                    static_cast<std::size_t>(
                        displayX);

                hits_[adjacentIndex] +=
                    fraction;
            }

            if (previousPlotY >= 0)
            {
                const int firstY =
                    std::min(
                        previousPlotY,
                        plotY);

                const int lastY =
                    std::max(
                        previousPlotY,
                        plotY);

                for (int y = firstY;
                    y <= lastY;
                    ++y)
                {
                    const std::size_t segmentIndex =
                        static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(
                            displayWidth) +
                        static_cast<std::size_t>(
                            displayX);

                    hits_[segmentIndex] +=
                        64u;
                }
            }

            previousPlotY = plotY;
        }
    }

    // Build the graticule first.  The all-lines density trace only writes
    // non-zero pixels afterwards, so the trace naturally sits on top.
    prepareImageForRender();
    image_.fill(Qt::black);
    {
        QPainter graticulePainter(&image_);
        graticule_.draw(
            graticulePainter,
            scope,
            VideoStandard::pal625());
    }

    const bool probePresentation =
        measurementProbePresentation_.load(
            std::memory_order_relaxed);

    double probeCenterX = 0.0;
    double probeCenterY = 0.0;
    double probeRadiusSquared = 0.0;

    if (probePresentation)
    {
        const AnalogVideoLevels probeAnalog =
            analogLevels(VideoColorStandard::Rec601_625);

        probeCenterX =
            scope.left() +
            measurementProbeNormalizedX_.load(
                std::memory_order_relaxed) *
                scope.width();

        probeCenterY =
            scope.bottom() -
            measurementProbeVolts_.load(
                std::memory_order_relaxed) *
                scope.height() /
                probeAnalog.graticuleMaxVolts;

        const double videoHeight =
            std::abs(
                (probeAnalog.whiteVolts - probeAnalog.blackVolts) *
                scope.height() /
                probeAnalog.graticuleMaxVolts);

        const double probeDiameter =
            (std::max)(12.0, videoHeight * 0.10);

        const double probeRadius =
            probeDiameter * 0.5;

        probeRadiusSquared =
            probeRadius * probeRadius;
    }

    for (int y = 0;
        y < displayHeight;
        ++y)
    {
        auto* destination =
            reinterpret_cast<QRgb*>(
                image_.scanLine(y));

        for (int x = 0;
            x < displayWidth;
            ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                static_cast<std::size_t>(
                    displayWidth) +
                static_cast<std::size_t>(x);

            const float persistence =
                static_cast<float>(persistence_) /
                255.0f;

            const float currentDensity =
                static_cast<float>(
                    hits_[index]);

            float& temporalDensity =
                allLinesPersistence_[index];

            if (persistence_ == 0)
            {
                temporalDensity =
                    currentDensity;
            }
            else
            {
                temporalDensity =
                    temporalDensity * persistence +
                    currentDensity;
            }

            const std::uint32_t tracePeakWhite =
                lineInfoOverlayPalOutput_
                ? 191u
                : 255u;

            std::uint32_t intensity =
                std::min<std::uint32_t>(
                    tracePeakWhite,
                    static_cast<std::uint32_t>(
                        temporalDensity / 256.0f) *
                    8u);

            if (probePresentation)
            {
                const double dx =
                    (static_cast<double>(x) + 0.5) -
                    probeCenterX;

                const double dy =
                    (static_cast<double>(y) + 0.5) -
                    probeCenterY;

                const bool insideProbe =
                    dx * dx + dy * dy <=
                    probeRadiusSquared;

                if (!insideProbe)
                {
                    intensity = (intensity + 1u) / 2u;
                }
            }

            if (intensity == 0)
            {
                continue;
            }

            destination[x] =
                qRgb(
                    static_cast<int>(intensity),
                    static_cast<int>(intensity),
                    static_cast<int>(intensity));
        }
    }

    renderTimings_.traceUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();
    QPainter painter(&image_);
    drawLineInfoOverlay(painter);
    renderTimings_.overlayUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);
}

void WaveformRenderer::addChromaFillPixel(
    int x,
    int y,
    int red,
    int green,
    int blue,
    int intensity)
{
    if (x < 0 ||
        x >= image_.width() ||
        y < 0 ||
        y >= image_.height())
    {
        return;
    }

    const std::size_t index =
        static_cast<std::size_t>(y) *
        static_cast<std::size_t>(
            image_.width()) +
        static_cast<std::size_t>(x);

    TracePixel& pixel =
        chromaTrace_[index];

    const auto addChannel =
        [intensity](
            std::uint16_t& destination,
            int channel)
        {
            const std::uint32_t contribution =
                static_cast<std::uint32_t>(
                    std::clamp(
                        channel,
                        0,
                        255)) *
                static_cast<std::uint32_t>(
                    intensity) /
                255u;

            destination =
                static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(
                        kMaximumChromaLevel,
                        static_cast<std::uint32_t>(
                            destination) +
                        contribution));
        };

    addChannel(
        pixel.red,
        red);

    addChannel(
        pixel.green,
        green);

    addChannel(
        pixel.blue,
        blue);
}

void WaveformRenderer::plotSegment(
    double x0,
    double y0,
    double x1,
    double y1,
    int intensity,
    int red,
    int green,
    int blue,
    int clipFirstX,
    int clipLastX)
{
    const double dx =
        x1 - x0;

    const double dy =
        y1 - y0;

    const double lengthSquared =
        dx * dx +
        dy * dy;

    if (lengthSquared <= 0.0)
    {
        plotBeam(
            x0,
            y0,
            intensity,
            red,
            green,
            blue,
            clipFirstX,
            clipLastX);

        return;
    }

    const double coreRadius =
        beamCoreRadiusPx_;

    // The segment bounds already use floor(min) / ceil(max), which
    // provide the sub-pixel guard band at both ends.  Using ceil()
    // here therefore adds an unnecessary full pixel whenever the
    // continuously scaled beam radius crosses an integer boundary
    // (for example 0.999 -> 1.001 px), causing a large raster-work
    // cliff without changing the visible beam support.  floor() keeps
    // the candidate box tight while still covering every integer pixel
    // whose centre can lie inside coreRadius.
    const int coreMargin =
        std::max(
            1,
            static_cast<int>(
                std::floor(coreRadius)));

    if (clipLastX < 0)
    {
        clipLastX = image_.width();
    }

    clipFirstX =
        std::clamp(
            clipFirstX,
            0,
            image_.width());

    clipLastX =
        std::clamp(
            clipLastX,
            clipFirstX,
            image_.width());

    const int firstX =
        std::max(
            clipFirstX,
            static_cast<int>(
                std::floor(
                    std::min(x0, x1))) -
                coreMargin);

    const int lastX =
        std::min(
            clipLastX - 1,
            static_cast<int>(
                std::ceil(
                    std::max(x0, x1))) +
                coreMargin);

    const int firstY =
        std::max(
            0,
            static_cast<int>(
                std::floor(
                    std::min(y0, y1))) -
                coreMargin);

    const int lastY =
        std::min(
            image_.height() - 1,
            static_cast<int>(
                std::ceil(
                    std::max(y0, y1))) +
                coreMargin);

    for (int y = firstY;
        y <= lastY;
        ++y)
    {
        for (int x = firstX;
            x <= lastX;
            ++x)
        {
            const double px =
                static_cast<double>(x);

            const double py =
                static_cast<double>(y);

            const double projection =
                ((px - x0) * dx +
                    (py - y0) * dy) /
                lengthSquared;

            const double t =
                std::clamp(
                    projection,
                    0.0,
                    1.0);

            const double nearestX =
                x0 + t * dx;

            const double nearestY =
                y0 + t * dy;

            const double distance =
                std::hypot(
                    px - nearestX,
                    py - nearestY);

            const double coverage =
                distance < coreRadius
                ? std::clamp(
                    1.0 -
                    (distance / coreRadius),
                    0.0,
                    1.0)
                : 0.0;

            if (coverage <= 0.0)
            {
                continue;
            }

            const std::uint32_t contribution =
                static_cast<std::uint32_t>(
                    coverage *
                    static_cast<double>(
                        intensity));

            const std::size_t index =
                static_cast<std::size_t>(y) *
                static_cast<std::size_t>(
                    image_.width()) +
                static_cast<std::size_t>(x);

            TracePixel& pixel =
                trace_[index];

            const auto addChannel =
                [contribution](
                    std::uint16_t& destination,
                    int channel)
                {
                    const std::uint32_t scaled =
                        contribution *
                        static_cast<std::uint32_t>(
                            std::clamp(
                                channel,
                                0,
                                255)) /
                        255u;

                    destination =
                        static_cast<std::uint16_t>(
                            std::min<std::uint32_t>(
                                65535u,
                                static_cast<std::uint32_t>(
                                    destination) +
                                scaled));
                };

            addChannel(
                pixel.red,
                red);

            addChannel(
                pixel.green,
                green);

            addChannel(
                pixel.blue,
                blue);
        }
    }
}

void WaveformRenderer::plotBeam(
    double x,
    double y,
    int intensity,
    int red,
    int green,
    int blue,
    int clipFirstX,
    int clipLastX)
{
    if (x < 0.0 ||
        x >= static_cast<double>(
            image_.width()))
    {
        return;
    }

    if (clipLastX < 0)
    {
        clipLastX = image_.width();
    }

    clipFirstX =
        std::clamp(
            clipFirstX,
            0,
            image_.width());

    clipLastX =
        std::clamp(
            clipLastX,
            clipFirstX,
            image_.width());

    const int x0 =
        static_cast<int>(
            std::floor(x));

    const int y0 =
        static_cast<int>(
            std::floor(y));

    const double fx =
        x -
        static_cast<double>(x0);

    const double fy =
        y -
        static_cast<double>(y0);

    for (int dy = 0;
        dy <= 1;
        ++dy)
    {
        const int destinationY =
            y0 + dy;

        if (destinationY < 0 ||
            destinationY >= image_.height())
        {
            continue;
        }

        const double wy =
            dy == 0
            ? 1.0 - fy
            : fy;

        for (int dx = 0;
            dx <= 1;
            ++dx)
        {
            const int destinationX =
                x0 + dx;

            if (destinationX < clipFirstX ||
                destinationX >= clipLastX)
            {
                continue;
            }

            const double wx =
                dx == 0
                ? 1.0 - fx
                : fx;

            const double weight =
                wx * wy;

            const std::uint32_t contribution =
                static_cast<std::uint32_t>(
                    weight *
                    static_cast<double>(
                        intensity));

            const std::size_t index =
                static_cast<std::size_t>(
                    destinationY) *
                static_cast<std::size_t>(
                    image_.width()) +
                static_cast<std::size_t>(
                    destinationX);

            TracePixel& pixel =
                trace_[index];

            const auto addChannel =
                [contribution](
                    std::uint16_t& destination,
                    int channel)
                {
                    const std::uint32_t scaled =
                        contribution *
                        static_cast<std::uint32_t>(
                            std::clamp(
                                channel,
                                0,
                                255)) /
                        255u;

                    destination =
                        static_cast<std::uint16_t>(
                            std::max<std::uint32_t>(
                                static_cast<std::uint32_t>(
                                    destination),
                                scaled));
                };

            addChannel(
                pixel.red,
                red);

            addChannel(
                pixel.green,
                green);

            addChannel(
                pixel.blue,
                blue);
        }
    }
}

void WaveformRenderer::setSelectedLine(
    int line)
{
    if (selectedLine_ == line)
    {
        return;
    }

    selectedLine_ = line;

    // A phosphor image belongs to exactly one selected source line.
    // Never feed the previous line into the first frame of the new line.
    clearScopephorFrames();

    // Also clear the legacy/current-frame trace buffers.
    std::fill(
        trace_.begin(),
        trace_.end(),
        TracePixel{});

    std::fill(
        chromaTrace_.begin(),
        chromaTrace_.end(),
        TracePixel{});
}

void WaveformRenderer::setPersistence(
    int persistence)
{
    const int normalizedPersistence =
        std::clamp(
            persistence,
            0,
            200);

    if (normalizedPersistence == 0 &&
        persistence_ != 0)
    {
        clearScopephorFrames();
    }

    persistence_ =
        normalizedPersistence;
}

void WaveformRenderer::setCoreIntensity(
    int intensity)
{
    const int normalizedIntensity =
        std::clamp(
            intensity,
            0,
            100);

    // UI 100% is the accepted High-Q beam level, historically 200%.
    coreIntensity_ =
        normalizedIntensity * 2;
}

void WaveformRenderer::setCoreWidth(
    int widthTenths)
{
    coreWidthTenths_ =
        std::clamp(
            widthTenths,
            5,
            30);
}



void WaveformRenderer::setAntiAliasing(
    bool enabled) noexcept
{
    antiAliasing_.store(
        enabled,
        std::memory_order_release);
}

void WaveformRenderer::setColorizeIllegalLuminance(
    bool enabled) noexcept
{
    colorizeIllegalLuminance_.store(
        enabled,
        std::memory_order_release);
}


void WaveformRenderer::setGlow(
    int glow)
{
    const int uiGlow =
        std::clamp(
            glow,
            0,
            100);

    // Keep the UI consistent at 0..100, but use only the visually useful
    // waveform Beam Glow range internally.  UI 50 is therefore the default
    // effective glow of 10, while UI 100 reaches the deliberately wild 20.
    glow_ =
        (uiGlow * 20 + 50) /
        100;
}

const QImage& WaveformRenderer::image() const
{
    return image_;
}

const std::vector<float>& WaveformRenderer::visibleLumaVolts() const noexcept
{
    return sourceY_;
}

const std::vector<float>& WaveformRenderer::fullLumaVolts() const noexcept
{
    return fullLumaVolts_;
}

const std::vector<float>& WaveformRenderer::reconstructedLumaSamples() const noexcept
{
    return singleLineReconstructed_;
}

void WaveformRenderer::setPreparedReconstructedLuma(
    const std::vector<float>* samples) noexcept
{
    preparedReconstructedLuma_ = samples;
}

const WaveformRenderTimings& WaveformRenderer::renderTimings() const noexcept
{
    return renderTimings_;
}

double WaveformRenderer::traceBandwidthMHz() const
{
    const double captureSampleRateMHz =
        inputSampleClockHz_ / 1'000'000.0;

    return
        captureSampleRateMHz *
        static_cast<double>(image_.width()) /
        (
            static_cast<double>(inputSampleWidth_) *
            kPixelsPerCycleForTraceBW);
}

void WaveformRenderer::clearTrace()
{
    clearScopephorFrames();

    std::fill(
        trace_.begin(),
        trace_.end(),
        TracePixel{});

    std::fill(
        chromaTrace_.begin(),
        chromaTrace_.end(),
        TracePixel{});
}


void WaveformRenderer::setAspectRatio(
    OpenScopeSettings::AspectRatio aspectRatio)
{
    if (aspectRatio_ == aspectRatio)
    {
        return;
    }

    aspectRatio_ = aspectRatio;

    clearTrace();
}