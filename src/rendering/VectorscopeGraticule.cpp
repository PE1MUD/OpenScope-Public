#include "VectorscopeGraticule.h"
#include "VectorscopeSettings.h"
#include "standards/ColorBars.h"
#include "standards/ColorMatrices.h"
#include <QColor>
#include <QPen>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <vector>

namespace
{

    struct Target
    {
        ColorBar color;
        double cb;
        double cr;
    };
    struct PolarTarget
    {
        double radius;
        double angleDegrees;
    };

    struct TargetTolerance
    {
        double hueDegrees;
        double amplitudePercent;
    };

    constexpr TargetTolerance kSmallTolerance
    {
        VectorscopeSettings::smallToleranceHueDegrees,
        VectorscopeSettings::smallToleranceAmplitudePercent
    };

    constexpr TargetTolerance kLargeTolerance
    {
        VectorscopeSettings::largeToleranceHueDegrees,
        VectorscopeSettings::largeToleranceAmplitudePercent
    };

    inline PolarTarget toPolar(
        const Target& target)
    {
        const double radius =
            std::hypot(
                target.cb,
                target.cr);

        const double angleDegrees =
            std::atan2(
                target.cr,
                target.cb) *
            180.0 /
            std::numbers::pi;

        return
        {
            radius,
            angleDegrees
        };
    }
    QPointF polarPoint(
        const QPointF& center,
        double radius,
        double angleDegrees)
    {
        const double angleRadians =
            angleDegrees *
            std::numbers::pi /
            180.0;

        return QPointF(
            center.x() +
            std::cos(angleRadians) * radius,
            center.y() -
            std::sin(angleRadians) * radius);
    }
    void drawToleranceTarget(
        QPainter& painter,
        const QPointF& center,
        double scopeRadius,
        const Target& target,
        const TargetTolerance& tolerance)
    {
        constexpr double kCbCrFullScale = 0.5;

        const PolarTarget polar =
            toPolar(target);

        const double nominalRadius =
            (polar.radius / kCbCrFullScale) *
            scopeRadius;

        const double amplitudeFraction =
            tolerance.amplitudePercent /
            100.0;

        const double radiusMin =
            nominalRadius *
            (1.0 - amplitudeFraction);

        const double radiusMax =
            nominalRadius *
            (1.0 + amplitudeFraction);

        const double angleMinDegrees =
            polar.angleDegrees -
            tolerance.hueDegrees;

        const double angleMaxDegrees =
            polar.angleDegrees +
            tolerance.hueDegrees;

        const QPointF p1 =
            polarPoint(
                center,
                radiusMin,
                angleMinDegrees);

        const QPointF p2 =
            polarPoint(
                center,
                radiusMax,
                angleMinDegrees);

        const QPointF p3 =
            polarPoint(
                center,
                radiusMax,
                angleMaxDegrees);

        const QPointF p4 =
            polarPoint(
                center,
                radiusMin,
                angleMaxDegrees);

        //painter.drawLine(
        //    p1,
        //    p2);

        //painter.drawLine(
        //    p2,
        //    p3);

        //painter.drawLine(
        //    p3,
        //    p4);

        //painter.drawLine(
        //    p4,
        //    p1);
        const bool largeTolerance =
            tolerance.amplitudePercent > 10.0;

        if (!largeTolerance)
        {
            // Small tolerance: closed box

            painter.drawLine(
                p1,
                p2);

            painter.drawLine(
                p2,
                p3);

            painter.drawLine(
                p3,
                p4);

            painter.drawLine(
                p4,
                p1);
        }
        else
        {
            // Large tolerance: outer corner marker

            constexpr double kCornerFraction = 0.15;

            auto drawCorner =
                [&](const QPointF& corner,
                    const QPointF& a,
                    const QPointF& b)
                {
                    painter.drawLine(
                        corner,
                        corner +
                        (a - corner) *
                        kCornerFraction);

                    painter.drawLine(
                        corner,
                        corner +
                        (b - corner) *
                        kCornerFraction);
                };



            drawCorner(
                p1,
                p2,
                p4);

            drawCorner(
                p2,
                p1,
                p3);

            drawCorner(
                p3,
                p2,
                p4);

            drawCorner(
                p4,
                p1,
                p3);
        }
    }
    constexpr Target makeTarget(
        ColorBar color,
        ColorBarLevel level,
        VideoColorStandard standard)
    {
        const CbCr cbcr =
            rgbToCbCr(
                colorBar(
                    color,
                    level),
                standard);

        return
        {
            color,
            cbcr.cb,
            cbcr.cr
        };
    }

    constexpr Target kTargets75[] =
    {
        makeTarget(
            ColorBar::Red,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Magenta,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Blue,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Cyan,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Green,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Yellow,
            ColorBarLevel::Percent75,
            VideoColorStandard::Rec601_625)
    };
    constexpr Target kTargets100[] =
    {
        makeTarget(
            ColorBar::Red,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Magenta,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Blue,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Cyan,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Green,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625),

        makeTarget(
            ColorBar::Yellow,
            ColorBarLevel::Percent100,
            VideoColorStandard::Rec601_625)
    };

    QColor targetColor(
        ColorBar color,
        ColorBarLevel level)
    {
        const Rgb rgb =
            colorBar(
                color,
                level);

        return QColor(
            static_cast<int>(rgb.r * 255.0),
            static_cast<int>(rgb.g * 255.0),
            static_cast<int>(rgb.b * 255.0));
    }

    double targetLabelAngularSide(ColorBar color)
    {
        switch (color)
        {
        case ColorBar::Red:
        case ColorBar::Green:
        case ColorBar::Magenta:
        case ColorBar::Cyan:
            return 1.0;

        case ColorBar::Yellow:
        case ColorBar::Blue:
            return -1.0;
        }

        return 1.0;
    }

    struct RingPoint
    {
        double angleRadians;
        QPointF point;
    };

    double normalizedAngleRadians(double angle)
    {
        constexpr double kTwoPi =
            2.0 * std::numbers::pi;

        while (angle < 0.0)
        {
            angle += kTwoPi;
        }

        while (angle >= kTwoPi)
        {
            angle -= kTwoPi;
        }

        return angle;
    }

    std::vector<RingPoint> makeRingPoints(
        const QPointF& center,
        const Target* targets,
        std::size_t targetCount,
        double radius)
    {
        constexpr double kCbCrFullScale = 0.5;

        std::vector<RingPoint> points;
        points.reserve(targetCount);

        for (std::size_t i = 0;
            i < targetCount;
            ++i)
        {
            const Target& target =
                targets[i];

            const double x =
                (target.cb / kCbCrFullScale) * radius;

            const double y =
                -(target.cr / kCbCrFullScale) * radius;

            points.push_back(
                {
                    normalizedAngleRadians(
                        std::atan2(-y, x)),
                    QPointF(
                        center.x() + x,
                        center.y() + y)
                });
        }

        std::sort(
            points.begin(),
            points.end(),
            [](const RingPoint& a,
                const RingPoint& b)
            {
                return a.angleRadians < b.angleRadians;
            });

        return points;
    }

    QPointF interpolateCentripetalCatmullRom(
        const QPointF& p0,
        const QPointF& p1,
        const QPointF& p2,
        const QPointF& p3,
        double t)
    {
        const auto distance =
            [](const QPointF& a,
                const QPointF& b)
            {
                const double dx = b.x() - a.x();
                const double dy = b.y() - a.y();
                return std::hypot(dx, dy);
            };

        const auto tj =
            [&](double ti,
                const QPointF& a,
                const QPointF& b)
            {
                return ti +
                    std::sqrt(
                        std::max(
                            distance(a, b),
                            1.0e-6));
            };

        const auto blend =
            [](const QPointF& a,
                const QPointF& b,
                double ta,
                double tb,
                double tSample)
            {
                const double denom =
                    std::max(tb - ta, 1.0e-6);

                const double wa =
                    (tb - tSample) / denom;

                const double wb =
                    (tSample - ta) / denom;

                return QPointF(
                    a.x() * wa + b.x() * wb,
                    a.y() * wa + b.y() * wb);
            };

        const double t0 = 0.0;
        const double t1 = tj(t0, p0, p1);
        const double t2 = tj(t1, p1, p2);
        const double t3 = tj(t2, p2, p3);

        const double sampleT =
            t1 + (t2 - t1) * t;

        const QPointF a1 =
            blend(
                p0,
                p1,
                t0,
                t1,
                sampleT);

        const QPointF a2 =
            blend(
                p1,
                p2,
                t1,
                t2,
                sampleT);

        const QPointF a3 =
            blend(
                p2,
                p3,
                t2,
                t3,
                sampleT);

        const QPointF b1 =
            blend(
                a1,
                a2,
                t0,
                t2,
                sampleT);

        const QPointF b2 =
            blend(
                a2,
                a3,
                t1,
                t3,
                sampleT);

        return blend(
            b1,
            b2,
            t1,
            t2,
            sampleT);
    }

    QPainterPath makeTargetReferenceRing(
        const QPointF& center,
        const Target* targets,
        std::size_t targetCount,
        double radius)
    {
        const std::vector<RingPoint> points =
            makeRingPoints(
                center,
                targets,
                targetCount,
                radius);

        QPainterPath path;

        if (points.size() < 3)
        {
            return path;
        }

        constexpr int kSamplesPerSegment = 48;

        bool firstPoint = true;

        for (std::size_t i = 0;
            i < points.size();
            ++i)
        {
            const std::size_t i0 =
                (i + points.size() - 1) % points.size();
            const std::size_t i1 = i;
            const std::size_t i2 =
                (i + 1) % points.size();
            const std::size_t i3 =
                (i + 2) % points.size();

            for (int sample = 0;
                sample < kSamplesPerSegment;
                ++sample)
            {
                const double t =
                    static_cast<double>(sample) /
                    static_cast<double>(kSamplesPerSegment);

                const QPointF p =
                    interpolateCentripetalCatmullRom(
                        points[i0].point,
                        points[i1].point,
                        points[i2].point,
                        points[i3].point,
                        t);

                if (firstPoint)
                {
                    path.moveTo(p);
                    firstPoint = false;
                }
                else
                {
                    path.lineTo(p);
                }
            }
        }

        path.closeSubpath();
        return path;
    }

    void drawTargetLevelHint(
        QPainter& painter,
        const QPointF& center,
        const Target& target,
        double radius,
        ColorBarLevel level,
        double lineWidth,
        double labelScale)
    {
        constexpr double kCbCrFullScale = 0.5;

        const QPointF position(
            center.x() +
            (target.cb / kCbCrFullScale) * radius,
            center.y() -
            (target.cr / kCbCrFullScale) * radius);

        const double nominalRadius =
            std::hypot(
                position.x() - center.x(),
                position.y() - center.y());

        const QString label =
            level == ColorBarLevel::Percent75
            ? QStringLiteral("75%")
            : QStringLiteral("100%");

        painter.save();

        QFont labelFont =
            painter.font();

        labelFont.setBold(true);
        labelFont.setPixelSize(
            static_cast<int>(
                radius *
                VectorscopeSettings::targetLabelSizeFraction *
                labelScale));

        painter.setFont(labelFont);
        painter.setPen(
            QPen(
                QColor(180, 150, 50),
                lineWidth));

        const QFontMetricsF metrics(
            painter.font());

        const QRectF textBounds =
            metrics.boundingRect(label);

        const double textHalfDiagonal =
            0.5 *
            std::hypot(
                textBounds.width(),
                textBounds.height());

        const double textHalfAngleRadians =
            std::asin(
                std::clamp(
                    textHalfDiagonal /
                    std::max(nominalRadius, 1.0),
                    0.0,
                    0.95));

        const double textHalfAngleDegrees =
            textHalfAngleRadians *
            180.0 /
            std::numbers::pi;

        constexpr double kLabelAngularGapDegrees = 2.5;

        // Cyan helper labels: keep their centers on exactly the nominal
        // 75% / 100% target radius, and move them only angularly until
        // the complete text clears the large tolerance corner sector.
        const double labelAngleDegrees =
            toPolar(target).angleDegrees -
            (kLargeTolerance.hueDegrees +
                textHalfAngleDegrees +
                kLabelAngularGapDegrees);

        const QPointF labelCenter =
            polarPoint(
                center,
                nominalRadius,
                labelAngleDegrees);

        const QRectF textRect(
            labelCenter.x() - textBounds.width() * 0.5,
            labelCenter.y() - textBounds.height() * 0.5,
            textBounds.width(),
            textBounds.height());

        painter.drawText(
            textRect,
            Qt::AlignCenter,
            label);

        painter.restore();
    }

    void drawTargets(
        QPainter& painter,
        const QPointF& center,
        double radius,
        const Target* targets,
        std::size_t targetCount,
        ColorBarLevel level,
        double lineWidth,
        double labelScale)
    {
        painter.setBrush(Qt::NoBrush);
        QFont labelFont =
            painter.font();

        labelFont.setBold(true);
        labelFont.setPixelSize(
            static_cast<int>(
                radius *
                VectorscopeSettings::targetLabelSizeFraction *
                labelScale));

        painter.setFont(labelFont);

        for (std::size_t i = 0;
            i < targetCount;
            ++i)
        {
            const Target& target =
                targets[i];

            painter.setPen(
                QPen(
                    targetColor(
                        target.color,
                        level),
                    lineWidth));
            constexpr double kCbCrFullScale = 0.5;

            const QPointF position(
                center.x() +
                (target.cb / kCbCrFullScale) * radius,
                center.y() -
                (target.cr / kCbCrFullScale) * radius);
            drawToleranceTarget(
                painter,
                center,
                radius,
                target,
                kLargeTolerance);
           
            drawToleranceTarget(
                painter,
                center,
                radius,
                target,
                kSmallTolerance);
            
            painter.setPen(
                QPen(
                    QColor(180, 150, 50),
                    lineWidth));

            if (level == ColorBarLevel::Percent100)
            {
                const double nominalRadius =
                    std::hypot(
                        position.x() - center.x(),
                        position.y() - center.y());

                const QString label =
                    colorBarShortName(target.color);

                // Never inherit a font modified by another annotation.
                // Every colour name uses exactly the same target-name font.
                painter.setFont(labelFont);

                const QFontMetricsF metrics(
                    painter.font());

                const QRectF textBounds =
                    metrics.boundingRect(label);

                // The label CENTER must stay exactly on the 100% colour
                // radius.  Move it only angularly along that circle until
                // the complete text clears the +/-10 degree large target
                // corner-marker sector.
                const double textHalfDiagonal =
                    0.5 *
                    std::hypot(
                        textBounds.width(),
                        textBounds.height());

                const double textHalfAngleRadians =
                    std::asin(
                        std::clamp(
                            textHalfDiagonal /
                            std::max(nominalRadius, 1.0),
                            0.0,
                            0.95));

                const double textHalfAngleDegrees =
                    textHalfAngleRadians *
                    180.0 /
                    std::numbers::pi;

                constexpr double kLabelAngularGapDegrees = 2.0;

                const double labelAngleDegrees =
                    toPolar(target).angleDegrees +
                    targetLabelAngularSide(target.color) *
                    (kLargeTolerance.hueDegrees +
                        textHalfAngleDegrees +
                        kLabelAngularGapDegrees);

                const QPointF labelCenter =
                    polarPoint(
                        center,
                        nominalRadius,
                        labelAngleDegrees);

                const QRectF textRect(
                    labelCenter.x() - textBounds.width() * 0.5,
                    labelCenter.y() - textBounds.height() * 0.5,
                    textBounds.width(),
                    textBounds.height());

                painter.drawText(
                    textRect,
                    Qt::AlignCenter,
                    label);
            }

            if (target.color == ColorBar::Cyan)
            {
                painter.setPen(
                    QPen(
                        QColor(180, 150, 50),
                        lineWidth));

                drawTargetLevelHint(
                    painter,
                    center,
                    target,
                    radius,
                    level,
                    lineWidth,
                    labelScale);
            }
        }
    }
}

void VectorscopeGraticule::setScale(double scale)
{
    scale_ = scale;
}

void VectorscopeGraticule::setLineWidth(double width)
{
    lineWidth_ = width;
}

void VectorscopeGraticule::setChromaMagnitude(double normalizedMagnitude)
{
    chromaMagnitude_ =
        std::clamp(
            normalizedMagnitude,
            0.0,
            1.20);
}

void VectorscopeGraticule::drawAxes(
    QPainter& painter,
    const QPointF& center,
    double radius,
    double lineScale,
    double labelScale) const
{
    const double labelOffset =
        radius *
        VectorscopeSettings::axisLabelOffsetFraction;

    const double effectiveLineWidth =
        lineWidth_ * lineScale;

    const bool pcRender =
        lineScale <= 1.05;

    QPen graticulePen(
        pcRender
            ? QColor(255, 255, 255)
            : QColor(135, 135, 125),
        effectiveLineWidth);

    painter.setPen(graticulePen);
    painter.setBrush(Qt::NoBrush);

    // Outer circle.
    painter.drawEllipse(
        center,
        radius,
        radius);

    for (int angle = 0;
        angle < 360;
        angle += 10)
    {
        const double angleRadians =
            static_cast<double>(angle) *
            std::numbers::pi /
            180.0;

        const QPointF direction(
            std::cos(angleRadians),
            -std::sin(angleRadians));

        const QPointF outer =
            center +
            direction * radius;

        const double tickLength =
            radius *
            VectorscopeSettings::degreeTickLengthFraction *
            ((angle % 30 == 0) ? 2.0 : 1.0);

        const QPointF inner =
            center +
            direction *
            (radius - tickLength);

        painter.drawLine(
            inner,
            outer);
    }

    painter.setPen(
        QPen(
            QColor(90, 90, 85),
            effectiveLineWidth));

    // Horizontal axis.
    painter.drawLine(
        QPointF(
            center.x() - radius,
            center.y()),
        QPointF(
            center.x() + radius,
            center.y()));

    // Vertical axis.
    painter.drawLine(
        QPointF(
            center.x(),
            center.y() - radius),
        QPointF(
            center.x(),
            center.y() + radius));

    QFont axisFont =
        painter.font();
    axisFont.setBold(true);

    axisFont.setPixelSize(
        static_cast<int>(
            radius *
            VectorscopeSettings::axisLabelSizeFraction *
            labelScale));

    painter.setFont(axisFont);

    // All graticule letters use the same instrument yellow as the
    // waveform numeric scale.  The geometry remains neutral gray.
    painter.setPen(
        QPen(
            QColor(180, 150, 50),
            effectiveLineWidth));

    const QFontMetricsF metrics(
        painter.font());

    const QRectF uBounds =
        metrics.boundingRect("U");

    const QRectF vBounds =
        metrics.boundingRect("V");

    const QRectF uRect(
        center.x() +
        radius -
        labelOffset -
        uBounds.width(),
        center.y() -
        labelOffset -
        uBounds.height(),
        uBounds.width(),
        uBounds.height());;

    painter.drawText(
        uRect,
        Qt::AlignCenter,
        "U");

    const QRectF vRect(
        center.x() +
        labelOffset,
        center.y() -
        radius +
        labelOffset,
        vBounds.width(),
        vBounds.height());

    painter.drawText(
        vRect,
        Qt::AlignCenter,
        "V");


}

void VectorscopeGraticule::draw(
    QPainter& painter,
    const QRectF& scopeRect,
    double lineScale,
    double labelScale) const
{
    const QPointF center =
        scopeRect.center();

    const double radius =
        scopeRect.width() * 0.45 * scale_;
    
    const double axesRadius =
        radius * 1.2;

    drawAxes(
        painter,
        center,
        axesRadius,
        lineScale,
        labelScale);

    // 75% / 100% target reference rings must use the SAME radius as the
    // target geometry below. drawAxes() deliberately uses axesRadius =
    // radius * 1.2 for the outer scope circle and degree ticks, so drawing
    // these rings there scaled them 20% too large.
    {
        const double effectiveLineWidth =
            lineWidth_ * lineScale;

        QPen referenceRingPen(
            QColor(72, 72, 68),
            effectiveLineWidth);

        referenceRingPen.setStyle(Qt::DotLine);
        referenceRingPen.setCapStyle(Qt::RoundCap);

        painter.setPen(referenceRingPen);
        painter.setBrush(Qt::NoBrush);

        painter.drawPath(
            makeTargetReferenceRing(
                center,
                kTargets75,
                std::size(kTargets75),
                radius));

        painter.drawPath(
            makeTargetReferenceRing(
                center,
                kTargets100,
                std::size(kTargets100),
                radius));
    }

    // Chroma magnitude meter on the negative U axis.  This is a visual
    // saturation meter, not a true full-radius plot.  Therefore the 75% and
    // 100% markers live at half of those nominal magnitudes:
    // 75% -> 37.5% of the scope radius, 100% -> 50% of the scope radius.
    {
        const double effectiveLineWidth =
            lineWidth_ * lineScale;

        const bool pcRender =
            lineScale <= 1.05;

        const double meterLineWidth =
            effectiveLineWidth *
            (pcRender ? 2.0 : 1.0);

        QPen meterPen(
            QColor(180, 150, 50),
            meterLineWidth);

        meterPen.setCapStyle(Qt::FlatCap);

        painter.setPen(meterPen);
        painter.setBrush(Qt::NoBrush);

        const QPointF end100(
            center.x() - radius * 0.50,
            center.y());

        const QPointF mark75(
            center.x() - radius * 0.375,
            center.y());

        const double tickHalfBase =
            std::max(
                5.0,
                radius * 0.026);

        const double tickHalf =
            tickHalfBase * 2.0;

        painter.drawLine(
            QPointF(mark75.x(), mark75.y() - tickHalf),
            QPointF(mark75.x(), mark75.y() + tickHalf));

        painter.drawLine(
            QPointF(end100.x(), end100.y() - tickHalf),
            QPointF(end100.x(), end100.y() + tickHalf));

        // Max chroma on the selected line.  No shaft: just a filled
        // triangular pointer whose tip lands on the measured magnitude.
        // The meter intentionally uses half the normal vectorscope radius,
        // so 75% maps to 0.375 R and 100% maps to 0.500 R.
        const double pointerRadius =
            radius * 0.50 * chromaMagnitude_;

        const QPointF tip(
            center.x() - pointerRadius,
            center.y());

        const double arrowLength =
            std::max(
                16.0,
                radius * 0.064);

        const double arrowHalfHeight =
            std::max(
                10.0,
                radius * 0.040);

        QPolygonF arrow;
        arrow << tip
              << QPointF(
                    tip.x() + arrowLength,
                    tip.y() - arrowHalfHeight)
              << QPointF(
                    tip.x() + arrowLength,
                    tip.y() + arrowHalfHeight);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(180, 150, 50));
        painter.drawPolygon(arrow);

        // Where the pointer crosses a meter tick, redraw only the portion
        // inside the triangular pointer in 75% cyan. The rest of each
        // reference tick remains yellow, so the crossing is unambiguous.
        painter.save();

        QPainterPath arrowClip;
        arrowClip.addPolygon(arrow);
        arrowClip.closeSubpath();
        painter.setClipPath(
            arrowClip,
            Qt::IntersectClip);

        QPen interceptPen(
            targetColor(
                ColorBar::Cyan,
                ColorBarLevel::Percent75),
            meterLineWidth);
        interceptPen.setCapStyle(Qt::FlatCap);

        painter.setPen(interceptPen);
        painter.setBrush(Qt::NoBrush);

        painter.drawLine(
            QPointF(mark75.x(), mark75.y() - tickHalf),
            QPointF(mark75.x(), mark75.y() + tickHalf));

        painter.drawLine(
            QPointF(end100.x(), end100.y() - tickHalf),
            QPointF(end100.x(), end100.y() + tickHalf));

        painter.restore();
    }

    const double targetLineWidth =
        lineWidth_ * lineScale *
        (lineScale <= 1.05 ? 1.6 : 1.0);

    drawTargets(
        painter,
        center,
        radius,
        kTargets75,
        std::size(kTargets75),
        ColorBarLevel::Percent75,
        targetLineWidth,
        labelScale);

    drawTargets(
        painter,
        center,
        radius,
        kTargets100,
        std::size(kTargets100),
        ColorBarLevel::Percent100,
        targetLineWidth,
        labelScale);
}