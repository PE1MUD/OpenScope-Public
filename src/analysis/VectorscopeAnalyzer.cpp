#include "VectorscopeAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <QElapsedTimer>
#include <QDebug>
#include <QPainter>
#include <QPointF>


VectorscopeAnalyzer::VectorscopeAnalyzer()
    : QObject(nullptr),
    image_(1, 1, QImage::Format_RGB32),
    allLinesImage_(
        kAllLinesWidth,
        kAllLinesHeight,
        QImage::Format_RGB32)
{
    image_.fill(Qt::black);

    persistenceImage_ =
        QImage(
            image_.size(),
            QImage::Format_RGB32);

    persistenceImage_.fill(
        Qt::black);

    allLinesImage_.fill(Qt::black);
    allLinesDensity_.resize(
        static_cast<std::size_t>(
            kAllLinesWidth *
            kAllLinesHeight));

}
void VectorscopeAnalyzer::setOutputSize(
    int width,
    int height)
{
    width =
        std::max(width, 1);

    height =
        std::max(height, 1);

    if (image_.width() == width &&
        image_.height() == height)
    {
        return;
    }

    image_ =
        QImage(
            width,
            height,
            QImage::Format_RGB32);

    image_.fill(Qt::black);

    persistenceImage_ =
        QImage(
            width,
            height,
            QImage::Format_RGB32);

    persistenceImage_.fill(
        Qt::black);

}
void VectorscopeAnalyzer::analyze(const Yuv444Frame& frame)
{
    renderTimings_ = {};

    QElapsedTimer timer;
    timer.start();

    image_.fill(Qt::black);

    if (selectedLine_ < 0)
    {
        timer.restart();
        renderAllLines(frame);
        renderTimings_.traceUs =
            static_cast<std::uint64_t>(
                timer.nsecsElapsed() / 1000);
        return;
    }

    const double centerX =
        (image_.width() - 1) * 0.5;

    const double centerY =
        (image_.height() - 1) * 0.5;

    const double scale =
        static_cast<double>(
            std::min(image_.width(), image_.height())) *
        0.5 *
        scale_ /
        32768.0;

    const double scaleX =
        scale * geometryScaleX_;

    const double scaleY =
        scale * geometryScaleY_;

    std::size_t firstSample = 0;
    std::size_t lastSample = frame.u.size();

    if (selectedLine_ >= 0 &&
        selectedLine_ < frame.height)
    {
        const std::size_t lineStart =
            static_cast<std::size_t>(selectedLine_) *
            static_cast<std::size_t>(frame.width);

        const std::size_t sourceWidth =
            static_cast<std::size_t>(
                frame.width);

        std::size_t viewWidth =
            sourceWidth;

        std::size_t viewOffset =
            0u;

        if (horizontalZoomFactor_ > 1)
        {
            viewWidth =
                std::max<std::size_t>(
                    sourceWidth /
                        static_cast<std::size_t>(
                            horizontalZoomFactor_),
                    1u);

            const std::size_t maximumOffset =
                sourceWidth -
                viewWidth;

            viewOffset =
                static_cast<std::size_t>(
                    std::lround(
                        horizontalScrollPosition_ *
                        static_cast<double>(
                            maximumOffset)));
        }

        firstSample =
            lineStart +
            viewOffset;

        lastSample =
            std::min(
                lineStart +
                    sourceWidth,
                firstSample +
                    viewWidth);
    }

    renderTimings_.setupUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    double sumU = 0.0;
    double sumV = 0.0;
    std::size_t sampleCount = 0;
    std::uint16_t minU = 65535;
    std::uint16_t maxU = 0;
    std::uint16_t minV = 65535;
    std::uint16_t maxV = 0;

    constexpr double chromaCenter =
        32768.0;

    auto samplePoint =
        [&](std::size_t index)
        {
            const double u =
                static_cast<double>(frame.u[index]) -
                chromaCenter;

            const double v =
                static_cast<double>(frame.v[index]) -
                chromaCenter;

            return QPointF(
                centerX + u * scaleX,
                centerY - v * scaleY);
        };

    auto plotPoint =
        [&](double x, double y, double intensity)
        {
            constexpr double radius = 2.0;
            constexpr double sigma = 0.80;

            const int minX =
                static_cast<int>(std::floor(x - radius));

            const int maxX =
                static_cast<int>(std::ceil(x + radius));

            const int minY =
                static_cast<int>(std::floor(y - radius));

            const int maxY =
                static_cast<int>(std::ceil(y + radius));

            for (int iy = minY;
                iy <= maxY;
                ++iy)
            {
                if (iy < 0 ||
                    iy >= image_.height())
                {
                    continue;
                }

                auto* line =
                    reinterpret_cast<QRgb*>(
                        image_.scanLine(iy));

                for (int ix = minX;
                    ix <= maxX;
                    ++ix)
                {
                    if (ix < 0 ||
                        ix >= image_.width())
                    {
                        continue;
                    }

                    const double dx =
                        (static_cast<double>(ix) + 0.5) - x;

                    const double dy =
                        (static_cast<double>(iy) + 0.5) - y;

                    const double distanceSquared =
                        dx * dx +
                        dy * dy;

                    if (distanceSquared >
                        radius * radius)
                    {
                        continue;
                    }

                    const double weight =
                        std::exp(
                            -distanceSquared /
                            (2.0 * sigma * sigma));

                    const int green =
                        static_cast<int>(
                            intensity * weight);

                    const int oldGreen =
                        qGreen(line[ix]);

                    const int newGreen =
                        std::min(
                            255,
                            oldGreen + green);

                    line[ix] =
                        qRgb(
                            newGreen,
                            newGreen,
                            newGreen);
                }
            }


        };

    for (std::size_t i = firstSample;
        i < lastSample;
        ++i)
    {
        sumU += static_cast<double>(frame.u[i]);
        sumV += static_cast<double>(frame.v[i]);
        ++sampleCount;

        minU = std::min(minU, frame.u[i]);
        maxU = std::max(maxU, frame.u[i]);
        minV = std::min(minV, frame.v[i]);
        maxV = std::max(maxV, frame.v[i]);
    }

    renderTimings_.statisticsUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();

    std::size_t totalSubdivisions = 0;
    std::size_t plottedPoints = 0;

    if (lastSample - firstSample >= 4)
    {
        for (std::size_t i = firstSample + 1;
            i + 2 < lastSample;
            ++i)
        {
            const QPointF p0 =
                samplePoint(i - 1);

            const QPointF p1 =
                samplePoint(i);

            const QPointF p2 =
                samplePoint(i + 1);

            const QPointF p3 =
                samplePoint(i + 2);

            const double dx =
                p2.x() - p1.x();

            const double dy =
                p2.y() - p1.y();

            const double distance =
                std::hypot(dx, dy);

            constexpr double targetStepPixels =
                0.5;

            const int subdivisions =
                std::clamp(
                    static_cast<int>(
                        std::ceil(
                            distance /
                            targetStepPixels)),
                    4,
                    256);

            totalSubdivisions +=
                static_cast<std::size_t>(
                    subdivisions);

            constexpr double referenceSubdivisions =
                32.0;

            constexpr double referenceEnergy =
                150.0;

            constexpr double referenceSize =
                576.0;

            const double renderScale =
                static_cast<double>(
                    std::min(
                        image_.width(),
                        image_.height())) /
                referenceSize;

            const double beamEnergy =
                referenceEnergy *
                referenceSubdivisions *
                std::pow(
                    renderScale,
                    1.2) /
                static_cast<double>(
                    subdivisions);

            // A CRT beam deposits less energy per area while it is
            // travelling quickly between chroma states. Keep dense
            // colour clusters bright, but suppress long transitions.
            constexpr double transitionAttenuationStrength =
                0.05;

            const double motionAttenuation =
                1.0 /
                (
                    1.0 +
                    distance *
                    transitionAttenuationStrength
                );

            const double segmentBeamEnergy =
                beamEnergy *
                motionAttenuation;

            for (int step = 0;
                step < subdivisions;
                ++step)
            {
                const double t =
                    static_cast<double>(step) /
                    static_cast<double>(
                        subdivisions);

                const double t2 = t * t;
                const double t3 = t2 * t;

                const double x =
                    0.5 *
                    (
                        (2.0 * p1.x()) +
                        (-p0.x() + p2.x()) * t +
                        (2.0 * p0.x() -
                            5.0 * p1.x() +
                            4.0 * p2.x() -
                            p3.x()) * t2 +
                        (-p0.x() +
                            3.0 * p1.x() -
                            3.0 * p2.x() +
                            p3.x()) * t3
                        );

                const double y =
                    0.5 *
                    (
                        (2.0 * p1.y()) +
                        (-p0.y() + p2.y()) * t +
                        (2.0 * p0.y() -
                            5.0 * p1.y() +
                            4.0 * p2.y() -
                            p3.y()) * t2 +
                        (-p0.y() +
                            3.0 * p1.y() -
                            3.0 * p2.y() +
                            p3.y()) * t3
                        );

                plotPoint(
                    x,
                    y,
                    segmentBeamEnergy);

                ++plottedPoints;
            }
        }
    }

    renderTimings_.traceUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();
    applyGlowPostProcess();
    renderTimings_.glowUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);

    timer.restart();
    applyPersistence();
    renderTimings_.persistenceUs =
        static_cast<std::uint64_t>(
            timer.nsecsElapsed() / 1000);
}
const QImage& VectorscopeAnalyzer::image() const
{
    return image_;
}

const VectorscopeAnalyzerTimings& VectorscopeAnalyzer::renderTimings() const noexcept
{
    return renderTimings_;
}

void VectorscopeAnalyzer::setSelectedLine(int line)
{
    selectedLine_ = line;
}

void VectorscopeAnalyzer::renderSingleLine(
    const Yuv444Frame& frame)
{
    // Hier komt straks de bestaande Catmull/Gaussian rendercode.
}

void VectorscopeAnalyzer::renderAllLines(
    const Yuv444Frame& frame)
{
    std::fill(
        allLinesDensity_.begin(),
        allLinesDensity_.end(),
        0u);

    allLinesImage_.fill(Qt::black);

    const double centerX =
        (kAllLinesWidth - 1) * 0.5;

    const double centerY =
        (kAllLinesHeight - 1) * 0.5;

    const double scaleX =
        static_cast<double>(kAllLinesWidth) *
        0.5 *
        scale_ /
        32768.0 *
        geometryScaleX_;

    const double scaleY =
        static_cast<double>(kAllLinesHeight) *
        0.5 *
        scale_ /
        32768.0 *
        geometryScaleY_;

    constexpr double chromaCenter =
        32768.0;

    constexpr std::uint32_t segmentEnergy =
        256u;

    for (int line = 0;
        line < frame.height;
        ++line)
    {
        const std::size_t lineStart =
            static_cast<std::size_t>(line) *
            static_cast<std::size_t>(frame.width);

        for (int x = 0;
            x + 1 < frame.width;
            ++x)
        {
            const std::size_t i0 =
                lineStart +
                static_cast<std::size_t>(x);

            const std::size_t i1 =
                i0 + 1;

            const double u0 =
                static_cast<double>(frame.u[i0]) -
                chromaCenter;

            const double v0 =
                static_cast<double>(frame.v[i0]) -
                chromaCenter;

            const double u1 =
                static_cast<double>(frame.u[i1]) -
                chromaCenter;

            const double v1 =
                static_cast<double>(frame.v[i1]) -
                chromaCenter;

            const int ix0 =
                static_cast<int>(
                    std::lround(
                        centerX +
                        u0 * scaleX));

            const int iy0 =
                static_cast<int>(
                    std::lround(
                        centerY -
                        v0 * scaleY));

            const int ix1 =
                static_cast<int>(
                    std::lround(
                        centerX +
                        u1 * scaleX));

            const int iy1 =
                static_cast<int>(
                    std::lround(
                        centerY -
                        v1 * scaleY));

            accumulateLineSegmentInteger(
                ix0,
                iy0,
                ix1,
                iy1,
                segmentEnergy,
                kAllLinesWidth,
                kAllLinesHeight,
                allLinesDensity_);
        }
    }

    constexpr double whitePoint =
        200000.0;

    constexpr double gamma =
        0.25;

    constexpr double minimumVisible =
        0.08;

    for (int y = 0;
        y < kAllLinesHeight;
        ++y)
    {
        auto* outputLine =
            reinterpret_cast<QRgb*>(
                allLinesImage_.scanLine(y));

        for (int x = 0;
            x < kAllLinesWidth;
            ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                static_cast<std::size_t>(
                    kAllLinesWidth) +
                static_cast<std::size_t>(x);

            const std::uint32_t density =
                allLinesDensity_[index];

            if (density == 0)
            {
                outputLine[x] =
                    qRgb(0, 0, 0);

                continue;
            }

            const double normalized =
                std::min(
                    1.0,
                    static_cast<double>(density) /
                    whitePoint);

            const double corrected =
                minimumVisible +
                (1.0 - minimumVisible) *
                std::pow(
                    normalized,
                    gamma);

            const int green =
                static_cast<int>(
                    255.0 * corrected);

            outputLine[x] =
                qRgb(
                    green,
                    green,
                    green);
        }
    }

    const int outputWidth =
        image_.width();

    const int outputHeight =
        image_.height();

    const int scopeSize =
        std::min(
            outputWidth,
            outputHeight);

    const QImage scaledScope =
        allLinesImage_.scaled(
            scopeSize,
            scopeSize,
            Qt::IgnoreAspectRatio,
            Qt::FastTransformation);

    image_.fill(
        Qt::black);

    QPainter painter(
        &image_);

    painter.drawImage(
        (outputWidth - scopeSize) / 2,
        (outputHeight - scopeSize) / 2,
        scaledScope);
}

void VectorscopeAnalyzer::accumulateLineSegmentInteger(
    int x0,
    int y0,
    int x1,
    int y1,
    std::uint32_t energy,
    int width,
    int height,
    std::vector<std::uint32_t>& density)
{
    const int dx =
        std::abs(x1 - x0);

    const int dy =
        std::abs(y1 - y0);

    const int steps =
        std::max(dx, dy) + 1;

    const std::uint32_t pointEnergy =
        std::max(
            1u,
            energy /
            static_cast<std::uint32_t>(steps));

    const int sx =
        (x0 < x1) ? 1 : -1;

    const int sy =
        (y0 < y1) ? 1 : -1;

    int error =
        dx - dy;

    for (;;)
    {
        if (x0 >= 0 &&
            x0 < width &&
            y0 >= 0 &&
            y0 < height)
        {
            const std::size_t index =
                static_cast<std::size_t>(y0) *
                static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x0);

            density[index] +=
                pointEnergy;
        }

        if (x0 == x1 &&
            y0 == y1)
        {
            break;
        }

        const int error2 =
            error * 2;

        if (error2 > -dy)
        {
            error -= dy;
            x0 += sx;
        }

        if (error2 < dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}
std::uint32_t VectorscopeAnalyzer::accumulateLineSegment(
    double x0,
    double y0,
    double x1,
    double y1,
    std::uint32_t energy)
{
    const double dx = x1 - x0;
    const double dy = y1 - y0;

    const double length =
        std::max(
            std::abs(dx),
            std::abs(dy));

    const int steps =
        std::max(
            1,
            static_cast<int>(
                std::ceil(length)));
    const std::uint32_t pointEnergy =
        std::max(
            1u,
            energy /
            static_cast<std::uint32_t>(
                steps + 1));
    for (int step = 0;
        step <= steps;
        ++step)
    {
        const double t =
            static_cast<double>(step) /
            static_cast<double>(steps);

        const int x =
            static_cast<int>(
                std::lround(
                    x0 + dx * t));

        const int y =
            static_cast<int>(
                std::lround(
                    y0 + dy * t));

        if (x < 0 ||
            x >= image_.width() ||
            y < 0 ||
            y >= image_.height())
        {
            continue;
        }

        const std::size_t index =
            static_cast<std::size_t>(y) *
            static_cast<std::size_t>(image_.width()) +
            static_cast<std::size_t>(x);

        density_[index] += pointEnergy;
    }
    return static_cast<std::uint32_t>(steps + 1);
}

void VectorscopeAnalyzer::setPersistence(
    int persistence)
{
    persistence =
        std::clamp(
            persistence,
            0,
            200);

    if (persistence_ != persistence &&
        persistence == 0)
    {
        persistenceImage_.fill(
            Qt::black);
    }

    persistence_ =
        persistence;
}

void VectorscopeAnalyzer::applyGlowPostProcess()
{
    if (glow_ <= 0 || image_.isNull() ||
        image_.width() < 2 || image_.height() < 2)
    {
        return;
    }

    const int width = image_.width();
    const int height = image_.height();
    const int lowWidth = (width + 1) / 2;
    const int lowHeight = (height + 1) / 2;
    const std::size_t lowCount =
        static_cast<std::size_t>(lowWidth) *
        static_cast<std::size_t>(lowHeight);

    constexpr int kTileWidth = 64;
    constexpr int kTileHeight = 32;
    const int tilesX = (lowWidth + kTileWidth - 1) / kTileWidth;
    const int tilesY = (lowHeight + kTileHeight - 1) / kTileHeight;

    renderTimings_.glowTotalTiles =
        static_cast<std::uint32_t>(tilesX * tilesY);
    renderTimings_.glowHorizontalPass1Tiles = 0;
    renderTimings_.glowVerticalPass1Tiles = 0;
    renderTimings_.glowHorizontalPass2Tiles = 0;
    renderTimings_.glowVerticalPass2Tiles = 0;

    static thread_local std::vector<std::uint8_t> dirtyTiles;
    dirtyTiles.assign(
        static_cast<std::size_t>(renderTimings_.glowTotalTiles),
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

    for (int ly = 0; ly < lowHeight; ++ly)
    {
        const int y0 = ly * 2;
        const int y1 = std::min(y0 + 1, height - 1);
        const auto* line0 = reinterpret_cast<const QRgb*>(image_.constScanLine(y0));
        const auto* line1 = reinterpret_cast<const QRgb*>(image_.constScanLine(y1));

        for (int lx = 0; lx < lowWidth; ++lx)
        {
            const int x0 = lx * 2;
            const int x1 = std::min(x0 + 1, width - 1);
            const int value = std::max({
                qGreen(line0[x0]),
                qGreen(line0[x1]),
                qGreen(line1[x0]),
                qGreen(line1[x1])
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
                    ++renderTimings_.glowDirtyTiles;
                }
            }
        }
    }

    if (maxActiveX >= minActiveX && maxActiveY >= minActiveY)
    {
        renderTimings_.glowActiveX = minActiveX * 2;
        renderTimings_.glowActiveY = minActiveY * 2;
        renderTimings_.glowActiveWidth =
            std::min(width - renderTimings_.glowActiveX,
                (maxActiveX - minActiveX + 1) * 2);
        renderTimings_.glowActiveHeight =
            std::min(height - renderTimings_.glowActiveY,
                (maxActiveY - minActiveY + 1) * 2);
    }

    const double renderScale =
        static_cast<double>(std::min(width, height)) / 576.0;
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
    renderTimings_.glowHorizontalPass1Tiles = countActiveTiles(horizontalMask1);
    blurHorizontal(source, temp, horizontalMask1, horizontal1Activity);

    dilateTiles(
        horizontalMask1,
        horizontal1Activity,
        verticalMask1,
        0,
        verticalTileExpansion);
    renderTimings_.glowVerticalPass1Tiles = countActiveTiles(verticalMask1);
    blurVertical(temp, blur, verticalMask1, vertical1Activity);

    dilateTiles(
        verticalMask1,
        vertical1Activity,
        horizontalMask2,
        horizontalTileExpansion,
        0);
    renderTimings_.glowHorizontalPass2Tiles = countActiveTiles(horizontalMask2);
    blurHorizontal(blur, temp, horizontalMask2, horizontal2Activity);

    dilateTiles(
        horizontalMask2,
        horizontal2Activity,
        verticalMask2,
        0,
        verticalTileExpansion);
    renderTimings_.glowVerticalPass2Tiles = countActiveTiles(verticalMask2);
    blurVertical(temp, blur, verticalMask2, vertical2Activity);

    const int glowScale = std::clamp(glow_, 0, 100);

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
                auto* line = reinterpret_cast<QRgb*>(image_.scanLine(y));
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

                    const int value =
                        std::min(255, qGreen(line[x]) + contribution);
                    line[x] = qRgb(value, value, value);
                }
            }
        }
    }

}

void VectorscopeAnalyzer::setGlow(
    int glow)
{
    glow_ =
        std::clamp(
            glow,
            0,
            100);

}

void VectorscopeAnalyzer::applyPersistence()
{
    if (persistenceImage_.size() != image_.size() ||
        persistenceImage_.format() != QImage::Format_RGB32)
    {
        persistenceImage_ =
            QImage(
                image_.size(),
                QImage::Format_RGB32);

        persistenceImage_.fill(
            Qt::black);
    }

    if (persistence_ <= 0)
    {
        persistenceImage_ =
            image_.copy();

        return;
    }

    const std::uint32_t persistence =
        static_cast<std::uint32_t>(
            persistence_);

    for (int y = 0;
        y < image_.height();
        ++y)
    {
        auto* current =
            reinterpret_cast<QRgb*>(
                image_.scanLine(y));

        auto* previous =
            reinterpret_cast<QRgb*>(
                persistenceImage_.scanLine(y));

        for (int x = 0;
            x < image_.width();
            ++x)
        {
            const int currentGreen =
                qGreen(
                    current[x]);

            const int previousGreen =
                qGreen(
                    previous[x]);

            const int decayedGreen =
                static_cast<int>(
                    (static_cast<std::uint32_t>(
                        previousGreen) *
                        persistence) >>
                    8);

            const int outputGreen =
                std::min(
                    255,
                    currentGreen +
                    decayedGreen);

            const QRgb output =
                qRgb(
                    outputGreen,
                    outputGreen,
                    outputGreen);

            previous[x] =
                output;

            current[x] =
                output;
        }
    }
}


void VectorscopeAnalyzer::setHorizontalWindow(
    int zoomFactor,
    double scrollPosition)
{
    if (zoomFactor != 1 &&
        zoomFactor != 5 &&
        zoomFactor != 10)
    {
        zoomFactor = 1;
    }

    horizontalZoomFactor_ =
        zoomFactor;

    horizontalScrollPosition_ =
        std::clamp(
            scrollPosition,
            0.0,
            1.0);
}

void VectorscopeAnalyzer::setScale(double scale)
{
    scale_ = scale;
}

void VectorscopeAnalyzer::setGeometryScale(
    double horizontalScale,
    double verticalScale)
{
    geometryScaleX_ =
        std::max(0.01, horizontalScale);

    geometryScaleY_ =
        std::max(0.01, verticalScale);
}
