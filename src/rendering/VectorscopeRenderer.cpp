#include "rendering/VectorscopeRenderer.h"

#include "VectorscopeSettings.h"
#include "standards/VideoStandard.h"
#include "standards/ColorBars.h"
#include "standards/ColorMatrices.h"
#include "ui/ViewportOverlay.h"

#include <QPainter>
#include <QPainterPath>
#include <QElapsedTimer>
#include <QPointF>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace
{
    QRectF snapSquare(const QRectF& desired)
    {
        const int diameter =
            std::max(
                2,
                static_cast<int>(
                    std::floor(
                        std::min(
                            desired.width(),
                            desired.height()))));

        const QPointF center = desired.center();

        const double left =
            std::round(
                center.x() -
                static_cast<double>(diameter) * 0.5);

        const double top =
            std::round(
                center.y() -
                static_cast<double>(diameter) * 0.5);

        return QRectF(
            left,
            top,
            static_cast<double>(diameter),
            static_cast<double>(diameter));
    }

    double referenceChromaMagnitude16Bit(
        VideoColorStandard standard)
    {
        constexpr ColorBar referenceColors[] =
        {
            ColorBar::Yellow,
            ColorBar::Cyan,
            ColorBar::Green,
            ColorBar::Magenta,
            ColorBar::Red,
            ColorBar::Blue
        };

        double maximumMagnitude = 0.0;

        for (const ColorBar color : referenceColors)
        {
            const CbCr cbcr =
                rgbToCbCr(
                    colorBar(
                        color,
                        ColorBarLevel::Percent100),
                    standard);

            maximumMagnitude =
                std::max(
                    maximumMagnitude,
                    std::hypot(
                        cbcr.cb,
                        cbcr.cr));
        }

        // Digital legal-range chroma is 512 +/-448 in 10-bit video.
        // The internal frame stores those samples left-shifted by six bits.
        // rgbToCbCr() expresses Cb/Cr on the conventional +/-0.5 scale,
        // so derive the code-space radius from that exact mapping.
        constexpr double kNeutral10Bit = 512.0;
        constexpr double kMaximum10Bit = 960.0;
        constexpr double kInternalScale = 64.0;
        constexpr double kNormalizedComponentFullScale = 0.5;

        const double codeUnitsPerCbCrUnit =
            ((kMaximum10Bit - kNeutral10Bit) * kInternalScale) /
            kNormalizedComponentFullScale;

        return maximumMagnitude * codeUnitsPerCbCrUnit;
    }

    struct GamutOffender
    {
        std::uint16_t u = 32768u;
        std::uint16_t v = 32768u;
    };

    struct GamutAnalysis
    {
        QVector<GamutOffender> offenders;

        [[nodiscard]] bool hasError() const noexcept
        {
            return !offenders.isEmpty();
        }
    };

    GamutAnalysis analyzePalCompositeGamut(
        const Yuv444Frame& frame,
        int selectedLine,
        int horizontalZoomFactor,
        double horizontalScrollPosition)
    {
        GamutAnalysis result;

        if (frame.width <= 0 ||
            frame.height <= 0 ||
            frame.y.empty() ||
            frame.u.empty() ||
            frame.v.empty())
        {
            return result;
        }

        const std::size_t sampleCount =
            std::min(
                frame.y.size(),
                std::min(
                    frame.u.size(),
                    frame.v.size()));

        std::size_t firstSample = 0;
        std::size_t lastSample = sampleCount;

        if (selectedLine >= 0 &&
            selectedLine < frame.height)
        {
            const std::size_t lineStart =
                static_cast<std::size_t>(selectedLine) *
                static_cast<std::size_t>(frame.width);

            const std::size_t sourceWidth =
                static_cast<std::size_t>(frame.width);

            std::size_t viewWidth = sourceWidth;
            std::size_t viewOffset = 0;

            if (horizontalZoomFactor > 1)
            {
                viewWidth =
                    std::max<std::size_t>(
                        sourceWidth /
                            static_cast<std::size_t>(horizontalZoomFactor),
                        1u);

                const std::size_t maximumOffset =
                    sourceWidth - viewWidth;

                viewOffset =
                    static_cast<std::size_t>(
                        std::lround(
                            std::clamp(
                                horizontalScrollPosition,
                                0.0,
                                1.0) *
                            static_cast<double>(maximumOffset)));
            }

            firstSample =
                std::min(
                    lineStart + viewOffset,
                    sampleCount);

            lastSample =
                std::min(
                    lineStart + sourceWidth,
                    std::min(
                        firstSample + viewWidth,
                        sampleCount));
        }

        // Rec.601 studio-range samples in the internal 16-bit container:
        // Y  : 64..940 (10-bit) -> 4096..60160
        // Cb/Cr: centre 512, legal excursion +/-448 -> centre 32768.
        constexpr double kYBlack = 64.0 * 64.0;
        constexpr double kYWhite = 940.0 * 64.0;
        constexpr double kYRange = kYWhite - kYBlack;
        constexpr double kChromaCenter = 512.0 * 64.0;
        constexpr double kChromaRange = 896.0 * 64.0;

        // PAL composite modulation from Rec.601 Cb/Cr.
        // B-Y = 1.772 Cb; R-Y = 1.402 Cr.
        // PAL U = 0.493(B-Y), V = 0.877(R-Y).
        constexpr double kPalUFromCb = 0.493 * 1.772;
        constexpr double kPalVFromCr = 0.877 * 1.402;

        // 100% PAL colour bars nominally reach about -33.4% / +133.4%.
        constexpr double kCompositeMinimum = -0.34;
        constexpr double kCompositeMaximum = 1.34;
        constexpr double kDecisionMargin = 0.02;

        // Reject transition ringing exactly as the existing alarm did.
        constexpr std::size_t kRequiredStableSamples = 24u;
        constexpr std::size_t kRequiredConsecutiveOutside = 8u;
        constexpr double kStableYDelta = 0.025;
        constexpr double kStableChromaDelta = 0.025;

        std::size_t stableSamples = 0u;
        std::size_t consecutiveOutside = 0u;
        std::size_t outsideRunStart = 0u;
        bool outsideRunConfirmed = false;

        double previousY = 0.0;
        double previousCb = 0.0;
        double previousCr = 0.0;
        bool havePrevious = false;

        const std::size_t lineWidth =
            static_cast<std::size_t>(
                std::max(frame.width, 1));

        auto appendOffender =
            [&](std::size_t index)
            {
                result.offenders.push_back(
                    GamutOffender{
                        frame.u[index],
                        frame.v[index]});
            };

        for (std::size_t i = firstSample;
            i < lastSample;
            ++i)
        {
            const double y =
                (static_cast<double>(frame.y[i]) - kYBlack) /
                kYRange;

            const double cb =
                (static_cast<double>(frame.u[i]) - kChromaCenter) /
                kChromaRange;

            const double cr =
                (static_cast<double>(frame.v[i]) - kChromaCenter) /
                kChromaRange;

            const bool lineStart =
                (i % lineWidth) == 0u;

            bool stable = false;

            if (havePrevious &&
                !lineStart)
            {
                stable =
                    std::abs(y - previousY) <= kStableYDelta &&
                    std::abs(cb - previousCb) <= kStableChromaDelta &&
                    std::abs(cr - previousCr) <= kStableChromaDelta;
            }

            previousY = y;
            previousCb = cb;
            previousCr = cr;
            havePrevious = true;

            if (!stable)
            {
                stableSamples = 0u;
                consecutiveOutside = 0u;
                outsideRunConfirmed = false;
                continue;
            }

            ++stableSamples;

            if (stableSamples < kRequiredStableSamples)
            {
                consecutiveOutside = 0u;
                outsideRunConfirmed = false;
                continue;
            }

            const double palU =
                kPalUFromCb * cb;

            const double palV =
                kPalVFromCr * cr;

            const double chromaPeak =
                std::hypot(
                    palU,
                    palV);

            const double compositeMinimum =
                y - chromaPeak;

            const double compositeMaximum =
                y + chromaPeak;

            const bool outside =
                compositeMinimum <
                    (kCompositeMinimum - kDecisionMargin) ||
                compositeMaximum >
                    (kCompositeMaximum + kDecisionMargin);

            if (!outside)
            {
                consecutiveOutside = 0u;
                outsideRunConfirmed = false;
                continue;
            }

            if (consecutiveOutside == 0u)
            {
                outsideRunStart = i;
            }

            ++consecutiveOutside;

            if (!outsideRunConfirmed &&
                consecutiveOutside >=
                    kRequiredConsecutiveOutside)
            {
                // The exact same sustained-run condition that used to make
                // GAMUT ERROR true now promotes the complete run to offender
                // status, including the seven preceding outside samples.
                for (std::size_t offenderIndex = outsideRunStart;
                    offenderIndex <= i;
                    ++offenderIndex)
                {
                    appendOffender(offenderIndex);
                }

                outsideRunConfirmed = true;
            }
            else if (outsideRunConfirmed)
            {
                appendOffender(i);
            }
        }

        return result;
    }

    void drawGamutOffenders(
        QPainter& painter,
        const QRectF& scopeRect,
        const GamutAnalysis& gamutAnalysis,
        bool videoProfile)
    {
        if (gamutAnalysis.offenders.isEmpty())
        {
            return;
        }

        const int scopeWidth =
            std::max(
                1,
                static_cast<int>(scopeRect.width()));

        const int scopeHeight =
            std::max(
                1,
                static_cast<int>(scopeRect.height()));

        const double centerX =
            scopeRect.left() +
            static_cast<double>(scopeWidth - 1) * 0.5;

        const double centerY =
            scopeRect.top() +
            static_cast<double>(scopeHeight - 1) * 0.5;

        const double scale =
            static_cast<double>(
                std::min(scopeWidth, scopeHeight)) *
            0.5 *
            VectorscopeSettings::scale *
            (36.0 / 35.0) /
            32768.0;

        const double scaleX =
            scale *
            (videoProfile
                ? 1.0 / VideoStandard::pal625().pixelAspectRatio
                : 1.0);

        const double scaleY = scale;

        QVector<QPointF> points;
        points.reserve(gamutAnalysis.offenders.size());

        constexpr double kChromaCenter = 32768.0;

        for (const GamutOffender& offender :
            gamutAnalysis.offenders)
        {
            const double u =
                static_cast<double>(offender.u) -
                kChromaCenter;

            const double v =
                static_cast<double>(offender.v) -
                kChromaCenter;

            points.push_back(
                QPointF(
                    centerX + u * scaleX,
                    centerY - v * scaleY));
        }

        painter.save();
        painter.setCompositionMode(
            QPainter::CompositionMode_SourceOver);

        QPen pen(QColor(255, 48, 48));
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawPoints(
            points.constData(),
            static_cast<int>(points.size()));
        painter.restore();
    }

    void drawGamutStatus(
        QPainter& painter,
        const QRectF& scopeRect,
        bool gamutError,
        bool videoProfile,
        const QRectF& bottomRightCard)
    {
        const QString text =
            QStringLiteral("GAMUT ERROR");

        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(
            std::max(
                11,
                static_cast<int>(
                    std::lround(
                        scopeRect.height() *
                        (videoProfile ? 0.026 : 0.024)))));

        painter.save();
        painter.setFont(font);

        QPainterPath textPath;
        const QFontMetricsF metrics(font);
        const QRectF bounds = metrics.boundingRect(text);

        const double inset =
            std::max(
                8.0,
                scopeRect.height() * 0.018);

        double rightEdge =
            scopeRect.right() - inset;

        double baselineY =
            scopeRect.bottom() - inset;

        if (videoProfile &&
            bottomRightCard.isValid() &&
            !bottomRightCard.isEmpty())
        {
            // Spout/video profile:
            // place GAMUT ERROR directly above the PROCESSING/YUV card,
            // aligned to the card's right edge.
            const double cardGap =
                std::max(
                    6.0,
                    scopeRect.height() * 0.012);

            rightEdge =
                bottomRightCard.right();

            baselineY =
                bottomRightCard.top() -
                cardGap -
                bounds.bottom();
        }

        const QPointF baseline(
            rightEdge - bounds.right(),
            baselineY);

        textPath.addText(
            baseline,
            font,
            text);

        if (gamutError)
        {
            QPen outerGlow(
                QColor(0, 255, 255, 45),
                std::max(4.0, scopeRect.height() * 0.010));
            outerGlow.setJoinStyle(Qt::RoundJoin);
            painter.setPen(outerGlow);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(textPath);

            QPen innerGlow(
                QColor(0, 255, 255, 110),
                std::max(2.0, scopeRect.height() * 0.005));
            innerGlow.setJoinStyle(Qt::RoundJoin);
            painter.setPen(innerGlow);
            painter.drawPath(textPath);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 255, 255));
            painter.drawPath(textPath);
        }
        else
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(80, 80, 80));
            painter.drawPath(textPath);
        }

        painter.restore();
    }
}

VectorscopeRenderer::VectorscopeRenderer(Profile profile)
    : profile_(profile)
{
    image_.fill(Qt::black);
    publishedImage_.fill(Qt::black);

    graticule_.setScale(
        VectorscopeSettings::scale);

    graticule_.setLineWidth(
        VectorscopeSettings::graticuleLineWidth);

    // Match analyzer legal-range chroma geometry to the graticule.
    // The analyzer uses a 0.5 image radius and 10-bit legal chroma has
    // +/-448 codes around 512, while the graticule uses a 0.45 radius.
    // 0.5 * (448/512) * (36/35) == 0.45 exactly.
    // Use the same calibration for both PC and Video/Spout renderers.
    analyzer_.setScale(
        VectorscopeSettings::scale * (36.0 / 35.0));

    if (profile_ == Profile::Video)
    {
        constexpr VideoStandard videoStandard =
            VideoStandard::pal625();

        analyzer_.setGeometryScale(
            1.0 / videoStandard.pixelAspectRatio,
            1.0);
    }
}

void VectorscopeRenderer::setOutputSize(int width, int height)
{
    outputWidth_ = std::max(width, 1);
    outputHeight_ = std::max(height, 1);

    if (image_.width() == outputWidth_ &&
        image_.height() == outputHeight_ &&
        publishedImage_.width() == outputWidth_ &&
        publishedImage_.height() == outputHeight_)
    {
        return;
    }

    image_ = QImage(
        outputWidth_,
        outputHeight_,
        QImage::Format_RGB32);

    publishedImage_ = QImage(
        outputWidth_,
        outputHeight_,
        QImage::Format_RGB32);

    image_.fill(Qt::black);
    publishedImage_.fill(Qt::black);
}

void VectorscopeRenderer::setContentScale(
    double horizontalScale,
    double verticalScale)
{
    contentScaleX_ = std::clamp(horizontalScale, 0.10, 1.0);
    contentScaleY_ = std::clamp(verticalScale, 0.10, 1.0);
}

void VectorscopeRenderer::setSelectedLine(int line)
{
    selectedLine_ = line;
    analyzer_.setSelectedLine(line);
}

void VectorscopeRenderer::setPersistence(int persistence)
{
    analyzer_.setPersistence(persistence);
}

void VectorscopeRenderer::setGlow(int glow)
{
    analyzer_.setGlow(glow);
}

void VectorscopeRenderer::setColorizeGamutErrors(
    bool enabled) noexcept
{
    colorizeGamutErrors_ = enabled;
}

void VectorscopeRenderer::setHorizontalWindow(
    int zoomFactor,
    double scrollPosition)
{
    if (zoomFactor != 1 &&
        zoomFactor != 5 &&
        zoomFactor != 10)
    {
        zoomFactor = 1;
    }

    horizontalZoomFactor_ = zoomFactor;
    horizontalScrollPosition_ =
        std::clamp(
            scrollPosition,
            0.0,
            1.0);

    analyzer_.setHorizontalWindow(
        zoomFactor,
        horizontalScrollPosition_);
}

void VectorscopeRenderer::setPresentationInfo(
    const VectorscopePresentationInfo& info)
{
    presentation_ = info;
}

void VectorscopeRenderer::moveAnalyzerToThread(QThread* thread)
{
    analyzer_.moveToThread(thread);
}

const QImage& VectorscopeRenderer::image() const
{
    return publishedImage_;
}

const VectorscopeRenderTimings& VectorscopeRenderer::renderTimings() const noexcept
{
    return renderTimings_;
}

QRectF VectorscopeRenderer::contentRect() const
{
    if (profile_ == Profile::Screen)
    {
        return QRectF(
            0.0,
            0.0,
            static_cast<double>(outputWidth_),
            static_cast<double>(outputHeight_));
    }

    return QRectF(
        (1.0 - contentScaleX_) * static_cast<double>(outputWidth_) * 0.5,
        (1.0 - contentScaleY_) * static_cast<double>(outputHeight_) * 0.5,
        contentScaleX_ * static_cast<double>(outputWidth_),
        contentScaleY_ * static_cast<double>(outputHeight_));
}

double VectorscopeRenderer::screenOwnerWidth(const QRectF& bounds) const
{
    const double ownerPadding =
        std::max(10.0, bounds.height() * 0.018);

    const QVector<ViewportOverlay::InfoRow> sourceRows
    {
        { QStringLiteral("SOURCE"), presentation_.source },
        { QStringLiteral("INPUT"), presentation_.input },
        { QStringLiteral("STANDARD"), presentation_.standard }
    };

    const QVector<ViewportOverlay::InfoRow> scopeRows
    {
        {
            QStringLiteral("LINE"),
            QStringLiteral("%1   X%2")
                .arg(
                    selectedLine_ >= 0
                    ? QString::number(selectedLine_)
                    : QStringLiteral("ALL"))
                .arg(horizontalZoomFactor_)
        },
        { QStringLiteral("TARGETS"), presentation_.targets },
        { QStringLiteral("MATRIX"), presentation_.matrix }
    };

    const QVector<ViewportOverlay::InfoRow> processingRows
    {
        { QStringLiteral("PROCESSING"), QString() },
        { presentation_.processing, QString() }
    };

    const QSizeF sourceSize =
        ViewportOverlay::infoCardRequiredSize(
            sourceRows,
            bounds.height(),
            false);

    const QSizeF scopeSize =
        ViewportOverlay::infoCardRequiredSize(
            scopeRows,
            bounds.height(),
            false);

    const QSizeF processingSize =
        ViewportOverlay::infoCardRequiredSize(
            processingRows,
            bounds.height(),
            false);

    const double requiredCardWidth =
        std::max(
            sourceSize.width(),
            std::max(
                scopeSize.width(),
                processingSize.width()));

    return requiredCardWidth + 2.0 * ownerPadding;
}

QRectF VectorscopeRenderer::screenScopeRect(const QRectF& bounds) const
{
    return snapSquare(
        ViewportOverlay::vectorscopeScopeRect(
            bounds,
            false,
            screenOwnerWidth(bounds)));
}

QRectF VectorscopeRenderer::videoScopeRect(
    const QRectF& bounds,
    QRectF* topLeftCard,
    QRectF* topRightCard,
    QRectF* bottomLeftCard,
    QRectF* bottomRightCard) const
{
    const double cardGap =
        std::max(6.0, bounds.height() * 0.012);

    const QVector<ViewportOverlay::InfoRow> topLeftRows
    {
        { presentation_.source, QString() },
        { presentation_.input, QString() }
    };

    const QVector<ViewportOverlay::InfoRow> topRightRows
    {
        { presentation_.standard, QString() },
        { presentation_.matrix, QString() }
    };

    const QVector<ViewportOverlay::InfoRow> bottomLeftRows
    {
        {
            QStringLiteral("LINE"),
            QStringLiteral("%1   X%2")
                .arg(
                    selectedLine_ >= 0
                    ? QString::number(selectedLine_)
                    : QStringLiteral("ALL"))
                .arg(horizontalZoomFactor_)
        }
    };

    const QVector<ViewportOverlay::InfoRow> bottomRightRows
    {
        { presentation_.processing, QString() }
    };

    const QSizeF topLeftSize =
        ViewportOverlay::infoCardRequiredSize(
            topLeftRows,
            bounds.height(),
            true);

    const QSizeF topRightSize =
        ViewportOverlay::infoCardRequiredSize(
            topRightRows,
            bounds.height(),
            true);

    const QSizeF bottomLeftSize =
        ViewportOverlay::infoCardRequiredSize(
            bottomLeftRows,
            bounds.height(),
            true);

    const QSizeF bottomRightSize =
        ViewportOverlay::infoCardRequiredSize(
            bottomRightRows,
            bounds.height(),
            true);

    *topLeftCard = QRectF(
        bounds.left(),
        bounds.top(),
        topLeftSize.width(),
        topLeftSize.height());

    *topRightCard = QRectF(
        bounds.right() - topRightSize.width(),
        bounds.top(),
        topRightSize.width(),
        topRightSize.height());

    *bottomLeftCard = QRectF(
        bounds.left(),
        bounds.bottom() - bottomLeftSize.height(),
        bottomLeftSize.width(),
        bottomLeftSize.height());

    *bottomRightCard = QRectF(
        bounds.right() - bottomRightSize.width(),
        bounds.bottom() - bottomRightSize.height(),
        bottomRightSize.width(),
        bottomRightSize.height());

    const QPointF center = bounds.center();

    auto distanceToCenter =
        [&center](const QPointF& point)
        {
            return std::hypot(
                point.x() - center.x(),
                point.y() - center.y());
        };

    double radius =
        std::min(bounds.width() * 0.5, bounds.height() * 0.5);

    radius = std::min(
        radius,
        distanceToCenter(
            QPointF(
                topLeftCard->right() + cardGap,
                topLeftCard->bottom() + cardGap)));

    radius = std::min(
        radius,
        distanceToCenter(
            QPointF(
                topRightCard->left() - cardGap,
                topRightCard->bottom() + cardGap)));

    radius = std::min(
        radius,
        distanceToCenter(
            QPointF(
                bottomLeftCard->right() + cardGap,
                bottomLeftCard->top() - cardGap)));

    radius = std::min(
        radius,
        distanceToCenter(
            QPointF(
                bottomRightCard->left() - cardGap,
                bottomRightCard->top() - cardGap)));

    radius = std::max(1.0, radius - 1.0);

    const int diameter =
        std::max(
            2,
            2 * static_cast<int>(std::floor(radius)));

    const double snappedRadius =
        static_cast<double>(diameter) * 0.5;

    return QRectF(
        std::round(center.x() - snappedRadius),
        std::round(center.y() - snappedRadius),
        static_cast<double>(diameter),
        static_cast<double>(diameter));
}

void VectorscopeRenderer::analyze(const Yuv444Frame& frame)
{
    renderTimings_ = {};

    QElapsedTimer totalTimer;
    totalTimer.start();

    QElapsedTimer phaseTimer;
    phaseTimer.start();

    const QRectF bounds = contentRect();

    QRectF topLeftCard;
    QRectF topRightCard;
    QRectF bottomLeftCard;
    QRectF bottomRightCard;

    const QRectF scopeRect =
        profile_ == Profile::Screen
        ? screenScopeRect(bounds)
        : videoScopeRect(
            bounds,
            &topLeftCard,
            &topRightCard,
            &bottomLeftCard,
            &bottomRightCard);

    const int scopeWidth =
        std::max(1, static_cast<int>(scopeRect.width()));

    const int scopeHeight =
        std::max(1, static_cast<int>(scopeRect.height()));

    analyzer_.setOutputSize(
        scopeWidth,
        scopeHeight);

    const GamutAnalysis gamutAnalysis =
        analyzePalCompositeGamut(
            frame,
            selectedLine_,
            horizontalZoomFactor_,
            horizontalScrollPosition_);

    const bool gamutError =
        gamutAnalysis.hasError();

    // Maximum chroma magnitude on the selected line. Normalize against
    // the largest 100% BT.601 colour-bar vector computed from the exact same
    // colorBar()/rgbToCbCr() definitions used by the vectorscope targets.
    // This keeps the chroma meter and target geometry mathematically tied.
    double maximumChroma = 0.0;

    if (!frame.u.empty() &&
        frame.u.size() == frame.v.size())
    {
        std::size_t firstSample = 0;
        std::size_t lastSample = frame.u.size();

        if (selectedLine_ >= 0 &&
            selectedLine_ < frame.height)
        {
            firstSample =
                static_cast<std::size_t>(selectedLine_) *
                static_cast<std::size_t>(frame.width);

            lastSample =
                std::min(
                    frame.u.size(),
                    firstSample +
                        static_cast<std::size_t>(frame.width));
        }

        constexpr double chromaCenter = 32768.0;

        const double chromaFullScale =
            referenceChromaMagnitude16Bit(
                VideoColorStandard::Rec601_625);

        for (std::size_t i = firstSample;
            i < lastSample;
            ++i)
        {
            const double u =
                static_cast<double>(frame.u[i]) -
                chromaCenter;

            const double v =
                static_cast<double>(frame.v[i]) -
                chromaCenter;

            maximumChroma =
                std::max(
                    maximumChroma,
                    std::hypot(u, v) /
                        chromaFullScale);
        }
    }

    graticule_.setChromaMagnitude(
        maximumChroma);

    renderTimings_.analyzerStartUs =
        static_cast<std::uint64_t>(
            totalTimer.nsecsElapsed() / 1000);

    analyzer_.analyze(frame);

    const auto& analyzerTimings =
        analyzer_.renderTimings();

    renderTimings_.analyzerUs =
        analyzerTimings.setupUs +
        analyzerTimings.statisticsUs +
        analyzerTimings.traceUs;

    renderTimings_.glowPersistenceUs =
        analyzerTimings.glowUs +
        analyzerTimings.persistenceUs;

    const std::uint64_t analyzerEndUs =
        static_cast<std::uint64_t>(
            totalTimer.nsecsElapsed() / 1000);

    renderTimings_.glowPersistenceStartUs =
        analyzerEndUs >= renderTimings_.glowPersistenceUs
        ? analyzerEndUs - renderTimings_.glowPersistenceUs
        : renderTimings_.analyzerStartUs +
            renderTimings_.analyzerUs;

    renderTimings_.composeStartUs =
        static_cast<std::uint64_t>(
            totalTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();
    image_.fill(Qt::black);

    QPainter painter(&image_);

    painter.setRenderHint(
        QPainter::Antialiasing,
        true);

    if (profile_ == Profile::Video)
    {
        constexpr VideoStandard videoStandard =
            VideoStandard::pal625();

        const QPointF center = scopeRect.center();

        painter.save();
        painter.translate(center);
        painter.scale(
            1.0 / videoStandard.pixelAspectRatio,
            1.0);
        painter.translate(-center);

        graticule_.draw(
            painter,
            scopeRect,
            1.8,
            1.35);

        painter.restore();
    }
    else
    {
        graticule_.draw(
            painter,
            scopeRect,
            1.0,
            1.0);
    }

    if (!analyzer_.image().isNull() &&
        analyzer_.image().width() == scopeWidth &&
        analyzer_.image().height() == scopeHeight)
    {
        painter.save();
        painter.setCompositionMode(
            QPainter::CompositionMode_Screen);
        painter.drawImage(
            scopeRect.topLeft(),
            analyzer_.image());
        painter.restore();
    }

    if (colorizeGamutErrors_)
    {
        drawGamutOffenders(
            painter,
            scopeRect,
            gamutAnalysis,
            profile_ == Profile::Video);

        drawGamutStatus(
            painter,
            scopeRect,
            gamutError,
            profile_ == Profile::Video,
            bottomRightCard);
    }

    renderTimings_.composeUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    renderTimings_.overlayStartUs =
        static_cast<std::uint64_t>(
            totalTimer.nsecsElapsed() / 1000);

    phaseTimer.restart();

    if (profile_ == Profile::Screen)
    {
        composeScreen(painter, bounds, scopeRect);
    }
    else
    {
        composeVideo(
            painter,
            bounds,
            scopeRect,
            topLeftCard,
            topRightCard,
            bottomLeftCard,
            bottomRightCard);
    }

    renderTimings_.overlayUs =
        static_cast<std::uint64_t>(
            phaseTimer.nsecsElapsed() / 1000);

    // QPainter must no longer reference the render target when buffers swap.
    painter.end();

    // Publish the fully completed frame. The next generation renders into
    // the alternate QImage instead of modifying the image just handed to Qt.
    image_.swap(
        publishedImage_);
}

void VectorscopeRenderer::composeScreen(
    QPainter& painter,
    const QRectF& bounds,
    const QRectF& scopeRect)
{
    double cardGap =
        std::max(8.0, bounds.height() * 0.016);

    double ownerPadding =
        std::max(10.0, bounds.height() * 0.018);

    // Keep the primary information cards alive during resize. Processing is
    // optional, but SOURCE and LINE/TARGETS/MATRIX should not blink out when
    // the viewport briefly passes through a short intermediate geometry.
    double cardReferenceHeight = bounds.height();

    const QVector<ViewportOverlay::InfoRow> sourceRows
    {
        { QStringLiteral("SOURCE"), presentation_.source },
        { QStringLiteral("INPUT"), presentation_.input },
        { QStringLiteral("STANDARD"), presentation_.standard }
    };

    const QVector<ViewportOverlay::InfoRow> scopeRows
    {
        {
            QStringLiteral("LINE"),
            QStringLiteral("%1   X%2")
                .arg(
                    selectedLine_ >= 0
                    ? QString::number(selectedLine_)
                    : QStringLiteral("ALL"))
                .arg(horizontalZoomFactor_)
        },
        { QStringLiteral("TARGETS"), presentation_.targets },
        { QStringLiteral("MATRIX"), presentation_.matrix }
    };

    const QVector<ViewportOverlay::InfoRow> processingRows
    {
        { QStringLiteral("PROCESSING"), QString() },
        { presentation_.processing, QString() }
    };

    QSizeF sourceSize =
        ViewportOverlay::infoCardRequiredSize(
            sourceRows,
            cardReferenceHeight,
            false);

    QSizeF scopeSize =
        ViewportOverlay::infoCardRequiredSize(
            scopeRows,
            cardReferenceHeight,
            false);

    QSizeF processingSize =
        ViewportOverlay::infoCardRequiredSize(
            processingRows,
            cardReferenceHeight,
            false);

    const double availableInfoHeight =
        std::max(1.0, bounds.height() - 16.0);

    auto mandatoryHeight = [&]()
    {
        return sourceSize.height() +
            cardGap +
            scopeSize.height() +
            2.0 * ownerPadding;
    };

    if (mandatoryHeight() > availableInfoHeight)
    {
        // Lose decorative breathing room before losing instrument state.
        ownerPadding = 3.0;
        cardGap = 3.0;

        // Then compact typography. The overlay helper keeps a readable
        // minimum font size, so this is deliberately bounded.
        while (mandatoryHeight() > availableInfoHeight &&
               cardReferenceHeight > 80.0)
        {
            cardReferenceHeight *= 0.90;

            sourceSize =
                ViewportOverlay::infoCardRequiredSize(
                    sourceRows,
                    cardReferenceHeight,
                    false);
            scopeSize =
                ViewportOverlay::infoCardRequiredSize(
                    scopeRows,
                    cardReferenceHeight,
                    false);
            processingSize =
                ViewportOverlay::infoCardRequiredSize(
                    processingRows,
                    cardReferenceHeight,
                    false);
        }
    }

    const double requiredCardWidth =
        std::max(
            sourceSize.width(),
            std::max(
                scopeSize.width(),
                processingSize.width()));

    const double measuredOwnerWidth =
        requiredCardWidth + 2.0 * ownerPadding;

    const QRectF infoRect =
        ViewportOverlay::vectorscopeInfoRect(
            bounds,
            false,
            measuredOwnerWidth);

    const double ownerWidth =
        std::min(infoRect.width(), measuredOwnerWidth);

    const double innerWidth =
        std::max(1.0, ownerWidth - 2.0 * ownerPadding);

    const bool mainGroupsFitWidth =
        sourceSize.width() <= innerWidth &&
        scopeSize.width() <= innerWidth;

    const bool processingFitsWidth =
        processingSize.width() <= innerWidth;

    const double minimumThreeCardHeight =
        sourceSize.height() +
        2.0 * cardGap +
        scopeSize.height() +
        processingSize.height();

    const bool processingFitsHeight =
        minimumThreeCardHeight +
        2.0 * ownerPadding <=
        infoRect.height();

    const bool drawProcessing =
        processingFitsWidth &&
        processingFitsHeight;

    const double minimumTwoCardHeight =
        sourceSize.height() +
        cardGap +
        scopeSize.height() +
        2.0 * ownerPadding;

    // Do not blank the complete information column during resize. The
    // adaptive compaction above keeps the mandatory cards present through
    // normal resize geometries; PROCESSING remains the first optional card.
    (void)mainGroupsFitWidth;
    (void)minimumTwoCardHeight;

    // Use the complete PC information column instead of shrinking the
    // owner panel around a compact stack.  This lets the information cards
    // read as three separate instrument blocks distributed over the height.
    const QRectF ownerRect(
        infoRect.left(),
        infoRect.top(),
        ownerWidth,
        infoRect.height());

    ViewportOverlay::drawOwnerPanel(
        painter,
        ownerRect,
        false);

    const double cardLeft =
        ownerRect.left() + ownerPadding;

    const double topY =
        ownerRect.top() + ownerPadding;

    const double bottomY =
        ownerRect.bottom() -
        ownerPadding -
        (drawProcessing
            ? processingSize.height()
            : scopeSize.height());

    const QRectF first(
        cardLeft,
        topY,
        innerWidth,
        sourceSize.height());

    ViewportOverlay::drawInfoCard(
        painter,
        first,
        sourceRows,
        cardReferenceHeight,
        false);

    if (drawProcessing)
    {
        const double middleY =
            ownerRect.center().y() -
            scopeSize.height() * 0.5;

        const QRectF second(
            cardLeft,
            middleY,
            innerWidth,
            scopeSize.height());

        ViewportOverlay::drawInfoCard(
            painter,
            second,
            scopeRows,
            cardReferenceHeight,
            false);

        const QRectF third(
            cardLeft,
            bottomY,
            innerWidth,
            processingSize.height());

        ViewportOverlay::drawInfoCard(
            painter,
            third,
            processingRows,
            cardReferenceHeight,
            false);
    }
    else
    {
        // Narrow/short fallback: keep the two mandatory groups separated
        // over the available height rather than returning to a top stack.
        const QRectF second(
            cardLeft,
            bottomY,
            innerWidth,
            scopeSize.height());

        ViewportOverlay::drawInfoCard(
            painter,
            second,
            scopeRows,
            cardReferenceHeight,
            false);
    }
}

void VectorscopeRenderer::composeVideo(
    QPainter& painter,
    const QRectF& bounds,
    const QRectF&,
    const QRectF& topLeftCard,
    const QRectF& topRightCard,
    const QRectF& bottomLeftCard,
    const QRectF& bottomRightCard)
{
    const QVector<ViewportOverlay::InfoRow> topLeftRows
    {
        { presentation_.source, QString() },
        { presentation_.input, QString() }
    };

    const QVector<ViewportOverlay::InfoRow> topRightRows
    {
        { presentation_.standard, QString() },
        { presentation_.matrix, QString() }
    };

    const QVector<ViewportOverlay::InfoRow> bottomLeftRows
    {
        {
            QStringLiteral("LINE"),
            QStringLiteral("%1   X%2")
                .arg(
                    selectedLine_ >= 0
                    ? QString::number(selectedLine_)
                    : QStringLiteral("ALL"))
                .arg(horizontalZoomFactor_)
        }
    };

    const QVector<ViewportOverlay::InfoRow> bottomRightRows
    {
        { presentation_.processing, QString() }
    };

    ViewportOverlay::drawInfoCard(
        painter,
        topLeftCard,
        topLeftRows,
        bounds.height(),
        true);

    ViewportOverlay::drawInfoCard(
        painter,
        topRightCard,
        topRightRows,
        bounds.height(),
        true);

    ViewportOverlay::drawInfoCard(
        painter,
        bottomLeftCard,
        bottomLeftRows,
        bounds.height(),
        true);

    ViewportOverlay::drawInfoCard(
        painter,
        bottomRightCard,
        bottomRightRows,
        bounds.height(),
        true);
}
