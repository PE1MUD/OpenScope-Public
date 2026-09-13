#include "PcmAudioScopeWidget.h"
#include "PcmAudioMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <QElapsedTimer>
#include <QFont>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QVector>
#include <QPointF>

class StereoPhaseDisplay final : public QWidget
{
public:
    explicit StereoPhaseDisplay(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(40, 40);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ageTimer_.start();
    }

    void setColorized(bool enabled)
    {
        if (colorized_ == enabled)
            return;
        colorized_ = enabled;
        phosphorImage_.fill(Qt::transparent);
        update();
    }

    void setStereoPcm16(const QByteArray& bytes)
    {
        if (bytes.size() < 4)
        {
            update();
            return;
        }

        const int sampleCount =
            bytes.size() / static_cast<int>(sizeof(std::int16_t));
        const int stereoFrames = sampleCount / 2;

        double maxVector = 0.0;

        constexpr double inv32768 = 1.0 / 32768.0;
        constexpr double invSqrt2 = 0.7071067811865475244;

        // Delta68: keep the complete time-ordered geometry long enough to
        // choose visually useful anchors.  Blind Nth-sample decimation throws
        // away the small loops/ellipses that make a goniometer informative.
        QVector<QPointF> rawPoints;
        rawPoints.reserve(stereoFrames);

        for (int frame = 0; frame < stereoFrames; ++frame)
        {
            std::int16_t leftSample = 0;
            std::int16_t rightSample = 0;
            const char* frameData =
                bytes.constData() + frame * 2 * static_cast<int>(sizeof(std::int16_t));
            std::memcpy(&leftSample, frameData, sizeof(leftSample));
            std::memcpy(&rightSample, frameData + sizeof(leftSample), sizeof(rightSample));

            const double l = static_cast<double>(leftSample) * inv32768;
            const double r = static_cast<double>(rightSample) * inv32768;
            // Broadcast audio-vectorscope convention: rotate L/R by 45 degrees
            // so in-phase mono is vertical.
            const double x = (r - l) * invSqrt2;
            const double y = (l + r) * invSqrt2;
            maxVector = std::max(maxVector, std::max(std::abs(x), std::abs(y)));

            rawPoints.push_back(QPointF(x, y));
        }

        // Geometry-aware temporal reduction.  Each short time bucket keeps
        // extrema in X/Y/radius plus the point with the strongest local turn.
        // That preserves little circles, ellipses and transients while removing
        // the redundant points that land on nearly the same straight trace.
        pendingPoints_.clear();
        constexpr int kGeometryBuckets = 44;
        const int rawCount = static_cast<int>(rawPoints.size());
        const int bucketSize = std::max(1,
            (rawCount + kGeometryBuckets - 1) / kGeometryBuckets);
        pendingPoints_.reserve(std::min(rawCount, kGeometryBuckets * 8 + 2));

        QVector<int> selected;
        selected.reserve(kGeometryBuckets * 8 + 2);
        auto addIndex = [&selected](int idx)
        {
            if (idx >= 0)
                selected.push_back(idx);
        };

        for (int begin = 0; begin < rawCount; begin += bucketSize)
        {
            const int end = std::min(rawCount, begin + bucketSize);
            int minX = begin, maxX = begin, minY = begin, maxY = begin;
            int maxR = begin, maxTurn = begin;
            double bestR2 = -1.0;
            double bestTurn = -1.0;

            for (int i = begin; i < end; ++i)
            {
                const QPointF& q = rawPoints[i];
                if (q.x() < rawPoints[minX].x()) minX = i;
                if (q.x() > rawPoints[maxX].x()) maxX = i;
                if (q.y() < rawPoints[minY].y()) minY = i;
                if (q.y() > rawPoints[maxY].y()) maxY = i;

                const double r2 = q.x() * q.x() + q.y() * q.y();
                if (r2 > bestR2)
                {
                    bestR2 = r2;
                    maxR = i;
                }

                if (i > 0 && i + 1 < rawCount)
                {
                    const QPointF a = rawPoints[i] - rawPoints[i - 1];
                    const QPointF b = rawPoints[i + 1] - rawPoints[i];
                    // Cross-product magnitude favours genuine local curvature;
                    // adding a small dot reversal term also retains hairpins.
                    const double cross = std::abs(a.x() * b.y() - a.y() * b.x());
                    const double dot = a.x() * b.x() + a.y() * b.y();
                    const double reversal = dot < 0.0 ? -0.35 * dot : 0.0;
                    const double turn = cross + reversal;
                    if (turn > bestTurn)
                    {
                        bestTurn = turn;
                        maxTurn = i;
                    }
                }
            }

            addIndex(begin);
            addIndex(minX); addIndex(maxX);
            addIndex(minY); addIndex(maxY);
            addIndex(maxR); addIndex(maxTurn);
            addIndex(end - 1);
        }
        if (!rawPoints.isEmpty())
            addIndex(rawCount - 1);

        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        for (int idx : selected)
            pendingPoints_.push_back(rawPoints[idx]);

        // A gentle auto-gain keeps quiet material readable without changing
        // the phase geometry. Reduce gain immediately, raise it gradually.
        const double targetGain =
            maxVector > 1.0e-5 ? std::clamp(0.88 / maxVector, 0.70, 18.0) : 1.0;
        if (targetGain < displayGain_)
            displayGain_ = targetGain;
        else
            displayGain_ += 0.12 * (targetGain - displayGain_);

        pendingPointsValid_ = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor bg(9, 13, 12);
        const QColor grid(118, 124, 122);
        const QColor circleGrid(255, 255, 255);
        const QColor text(255, 255, 255);
        const QColor green(32, 210, 86);

        p.fillRect(rect(), bg);

        const QRectF full = rect().adjusted(5.0, 5.0, -5.0, -5.0);
        const qreal side = std::max<qreal>(24.0, std::min(full.width(), full.height()));
        QRectF square(
            full.center().x() - side * 0.5,
            full.center().y() - side * 0.5,
            side,
            side);

        const QPointF c = square.center();
        const qreal radius = 0.46 * side;

        // Internal graticule first: the phosphor trace must remain visually
        // dominant over these subdued reference lines.
        p.setPen(QPen(grid, 1.35));
        p.drawLine(QPointF(c.x(), c.y() - radius), QPointF(c.x(), c.y() + radius));
        p.drawLine(QPointF(c.x() - radius, c.y()), QPointF(c.x() + radius, c.y()));
        p.drawLine(QPointF(c.x() - radius * 0.707, c.y() + radius * 0.707),
                   QPointF(c.x() + radius * 0.707, c.y() - radius * 0.707));
        p.drawLine(QPointF(c.x() - radius * 0.707, c.y() - radius * 0.707),
                   QPointF(c.x() + radius * 0.707, c.y() + radius * 0.707));

        updatePhosphor(square, c, radius, green);
        if (!phosphorImage_.isNull())
            p.drawImage(square.topLeft(), phosphorImage_);

        // Keep the outer reference circle and orientation labels crisp on top
        // of the phosphor.  This preserves Delta36's trace-over-grid look
        // without allowing a bright trace to erase Delta37's white outline
        // and MONO/L/R labels.
        p.setPen(QPen(circleGrid, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, radius, radius);

        QFont labelFont = font();
        labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 1.0));
        p.setFont(labelFont);
        p.setPen(text);

        // In the compact/quad presentation the labels need a little more air
        // around the circle.  Keep the established fullscreen placement, but
        // push L / MONO / R outward when the scope itself is small.
        const bool compactLabels = side < 360.0;
        const qreal labelGap = compactLabels ? 7.0 : 0.0;
        const qreal monoTop = compactLabels
            ? (c.y() - radius - labelGap - 18.0)
            : square.top();
        const qreal leftX = compactLabels
            ? (c.x() - radius - labelGap - 24.0)
            : square.left();
        const qreal rightX = compactLabels
            ? (c.x() + radius + labelGap)
            : (square.right() - 24.0);

        p.drawText(QRectF(c.x() - 28.0, monoTop, 56.0, 18.0), Qt::AlignCenter, QStringLiteral("MONO"));
        p.drawText(QRectF(leftX, c.y() - 14.0, 24.0, 20.0), Qt::AlignCenter, QStringLiteral("L"));
        p.drawText(QRectF(rightX, c.y() - 14.0, 24.0, 20.0), Qt::AlignCenter, QStringLiteral("R"));

    }

private:
    static constexpr double kPersistenceTauMs = 560.0;

    void updatePhosphor(
        const QRectF& square,
        const QPointF& c,
        qreal radius,
        const QColor& green)
    {
        const QSize imageSize(
            std::max(1, static_cast<int>(std::lround(square.width()))),
            std::max(1, static_cast<int>(std::lround(square.height()))));

        if (phosphorImage_.size() != imageSize ||
            phosphorImage_.format() != QImage::Format_ARGB32_Premultiplied)
        {
            phosphorImage_ = QImage(imageSize, QImage::Format_ARGB32_Premultiplied);
            phosphorImage_.fill(Qt::transparent);
            lastPhosphorMs_ = ageTimer_.elapsed();
        }

        const qint64 nowMs = ageTimer_.elapsed();
        const qint64 elapsedMs = std::max<qint64>(0, nowMs - lastPhosphorMs_);
        lastPhosphorMs_ = nowMs;

        // Exponential phosphor decay.  Fade the accumulated ARGB surface by
        // multiplying its alpha/energy; do NOT paint translucent black into it.
        // A black SourceOver fade makes untouched pixels increasingly opaque and
        // therefore erases the graticule underneath when this image is composed.
        if (elapsedMs > 0)
        {
            const double keep = std::exp(-static_cast<double>(elapsedMs) / kPersistenceTauMs);
            const int keepAlpha = std::clamp(
                static_cast<int>(std::lround(keep * 255.0)),
                0,
                255);
            if (keepAlpha < 255)
            {
                QPainter fade(&phosphorImage_);
                fade.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                fade.fillRect(phosphorImage_.rect(), QColor(255, 255, 255, keepAlpha));
            }
        }

        if (!pendingPointsValid_ || pendingPoints_.isEmpty())
            return;

        QPainter phosphor(&phosphorImage_);
        phosphor.setRenderHint(QPainter::Antialiasing, false);
        phosphor.setCompositionMode(QPainter::CompositionMode_Plus);

        const QPointF localCenter(
            c.x() - square.left(),
            c.y() - square.top());

        // Delta67: make the Audio Scope read like a real goniometer instead
        // of a soft point cloud.  Re-use the same decimated sample set and
        // accumulation image, but connect adjacent samples with a thin bright
        // core plus a very weak halo.  This costs roughly the same amount of
        // raster work as the previous two point passes and needs no blur,
        // supersampling or extra worker.
        QVector<QPointF> localPoints;
        localPoints.reserve(pendingPoints_.size());
        for (const QPointF& v : pendingPoints_)
        {
            localPoints.push_back(QPointF(
                localCenter.x() + v.x() * displayGain_ * radius,
                localCenter.y() - v.y() * displayGain_ * radius));
        }

        // Smooth the reduced time path with a cheap Catmull-Rom spline.
        // Three substeps per anchor interval is enough at normal monitor
        // refresh rates and is far cheaper than drawing every 44.1 kHz sample.
        QVector<QPointF> splinePoints;
        if (localPoints.size() >= 2)
        {
            constexpr int kSplineSubsteps = 3;
            splinePoints.reserve((localPoints.size() - 1) * kSplineSubsteps + 1);
            splinePoints.push_back(localPoints.front());

            for (int i = 0; i + 1 < localPoints.size(); ++i)
            {
                const QPointF& p0 = localPoints[std::max(0, i - 1)];
                const QPointF& p1 = localPoints[i];
                const QPointF& p2 = localPoints[i + 1];
                const int localCount = static_cast<int>(localPoints.size());
                const QPointF& p3 = localPoints[std::min(localCount - 1, i + 2)];

                for (int step = 1; step <= kSplineSubsteps; ++step)
                {
                    const double t = static_cast<double>(step) / kSplineSubsteps;
                    const double t2 = t * t;
                    const double t3 = t2 * t;
                    const double x = 0.5 * ((2.0 * p1.x()) +
                        (-p0.x() + p2.x()) * t +
                        (2.0 * p0.x() - 5.0 * p1.x() + 4.0 * p2.x() - p3.x()) * t2 +
                        (-p0.x() + 3.0 * p1.x() - 3.0 * p2.x() + p3.x()) * t3);
                    const double y = 0.5 * ((2.0 * p1.y()) +
                        (-p0.y() + p2.y()) * t +
                        (2.0 * p0.y() - 5.0 * p1.y() + 4.0 * p2.y() - p3.y()) * t2 +
                        (-p0.y() + 3.0 * p1.y() - 3.0 * p2.y() + p3.y()) * t3);
                    splinePoints.push_back(QPointF(x, y));
                }
            }
        }
        else
        {
            splinePoints = localPoints;
        }

        const QRectF imageBounds(phosphorImage_.rect());

        const auto traceColorForPoint =
            [this, &green, &localCenter, radius](const QPointF& pt)
            {
                if (!colorized_ || radius <= 0.0 || displayGain_ <= 0.0)
                    return green;

                // Convert display coordinates back to the normalized 45-degree
                // L/R vectors. This makes colour semantic rather than decorative:
                // left-heavy -> cyan, right-heavy -> amber, anti-phase -> red.
                const double x =
                    (pt.x() - localCenter.x()) / (displayGain_ * radius);
                const double y =
                    -(pt.y() - localCenter.y()) / (displayGain_ * radius);
                constexpr double invSqrt2 = 0.7071067811865475244;
                const double l = (y - x) * invSqrt2;
                const double r = (y + x) * invSqrt2;

                const double sumAxis = std::abs(y);
                const double diffAxis = std::abs(x);
                const double anti =
                    std::clamp(1.8 * (diffAxis - sumAxis) /
                        std::max(1.0e-9, diffAxis + sumAxis), 0.0, 1.0);

                const double lrDenom = std::max(1.0e-9, std::abs(l) + std::abs(r));
                const double balance =
                    std::clamp((std::abs(r) - std::abs(l)) / lrDenom, -1.0, 1.0);

                const QColor leftColor(70, 220, 255);
                const QColor rightColor(255, 205, 70);
                const QColor antiColor(255, 78, 68);

                const auto mix = [](const QColor& a, const QColor& b, double t)
                {
                    t = std::clamp(t, 0.0, 1.0);
                    return QColor(
                        static_cast<int>(std::lround(a.red()   + (b.red()   - a.red())   * t)),
                        static_cast<int>(std::lround(a.green() + (b.green() - a.green()) * t)),
                        static_cast<int>(std::lround(a.blue()  + (b.blue()  - a.blue())  * t)));
                };

                QColor base = green;
                if (balance < -0.08)
                    base = mix(green, leftColor, std::min(1.0, -balance));
                else if (balance > 0.08)
                    base = mix(green, rightColor, std::min(1.0, balance));

                return mix(base, antiColor, anti);
            };

        // Same cheap raster path as Delta68b. Colour changes only the pen; no
        // blur, supersampling, extra samples or additional worker are involved.
        for (int i = 1; i < splinePoints.size(); ++i)
        {
            const QPointF& a = splinePoints[i - 1];
            const QPointF& b = splinePoints[i];
            if (!(imageBounds.contains(a) || imageBounds.contains(b)))
                continue;
            QColor halo = traceColorForPoint((a + b) * 0.5);
            halo.setAlpha(16);
            phosphor.setPen(QPen(halo, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            phosphor.drawLine(a, b);
        }

        for (int i = 1; i < splinePoints.size(); ++i)
        {
            const QPointF& a = splinePoints[i - 1];
            const QPointF& b = splinePoints[i];
            if (!(imageBounds.contains(a) || imageBounds.contains(b)))
                continue;
            QColor core = traceColorForPoint((a + b) * 0.5);
            core.setAlpha(112);
            phosphor.setPen(QPen(core, 1.15, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            phosphor.drawLine(a, b);
        }

        // Sparse single-pixel energy keeps stationary/near-stationary material
        // visible without turning the trace back into a fuzzy cloud.
        for (int i = 0; i < localPoints.size(); i += 4)
        {
            if (!imageBounds.contains(localPoints[i]))
                continue;
            QColor pointColor = traceColorForPoint(localPoints[i]);
            pointColor.setAlpha(72);
            phosphor.setPen(QPen(pointColor, 1.0, Qt::SolidLine, Qt::RoundCap));
            phosphor.drawPoint(localPoints[i]);
        }

        pendingPointsValid_ = false;
    }

    QVector<QPointF> pendingPoints_;
    QImage phosphorImage_;
    QElapsedTimer ageTimer_;
    qint64 lastPhosphorMs_ = 0;
    double displayGain_ = 1.0;
    bool pendingPointsValid_ = false;
    bool colorized_ = true;
};


class StereoCorrelationMeter final : public QWidget
{
public:
    explicit StereoCorrelationMeter(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        // Keep the phase meter visually part of the Audio Scope rather than
        // squeezing it into a narrow, boxed-off strip at the far right.
        setMinimumWidth(58);
        setMaximumWidth(120);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }

    void setStereoPcm16(const QByteArray& bytes)
    {
        if (bytes.size() < 4)
        {
            correlation_ *= 0.85;
            update();
            return;
        }

        const int stereoFrames =
            bytes.size() / (2 * static_cast<int>(sizeof(std::int16_t)));
        double sumL2 = 0.0;
        double sumR2 = 0.0;
        double sumLR = 0.0;
        constexpr double inv32768 = 1.0 / 32768.0;

        for (int frame = 0; frame < stereoFrames; ++frame)
        {
            std::int16_t leftSample = 0;
            std::int16_t rightSample = 0;
            const char* frameData =
                bytes.constData() + frame * 2 * static_cast<int>(sizeof(std::int16_t));
            std::memcpy(&leftSample, frameData, sizeof(leftSample));
            std::memcpy(&rightSample, frameData + sizeof(leftSample), sizeof(rightSample));
            const double l = static_cast<double>(leftSample) * inv32768;
            const double r = static_cast<double>(rightSample) * inv32768;
            sumL2 += l * l;
            sumR2 += r * r;
            sumLR += l * r;
        }

        const double denom = std::sqrt(sumL2 * sumR2);
        const double blockCorrelation =
            denom > 1.0e-12 ? std::clamp(sumLR / denom, -1.0, 1.0) : 1.0;
        correlation_ = 0.82 * correlation_ + 0.18 * blockCorrelation;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), QColor(5, 5, 5));

        const QColor grid(118, 124, 122);
        const QColor text(238, 238, 235);
        const QColor positive(30, 150, 72);
        const QColor positiveBright(54, 225, 105);
        const QColor negative(205, 72, 62);

        const QRectF full = rect().adjusted(5.0, 7.0, -5.0, -7.0);
        const qreal labelHeight = 16.0;
        const qreal barWidth = std::clamp(full.width() * 0.34, 12.0, 30.0);
        const QRectF bar(
            full.center().x() - 0.5 * barWidth,
            full.top() + labelHeight,
            barWidth,
            std::max<qreal>(24.0, full.height() - 2.0 * labelHeight));

        p.setPen(QPen(grid, 1.0));
        p.setBrush(QColor(8, 8, 8));
        p.drawRect(bar);

        // Vertical correlation scale: +1 at the top, 0 in the centre, -1 bottom.
        const qreal zeroY = bar.center().y();
        p.drawLine(QPointF(bar.left() - 3.0, zeroY), QPointF(bar.right() + 3.0, zeroY));
        const qreal y = bar.bottom() -
            (correlation_ + 1.0) * 0.5 * bar.height();

        if (correlation_ >= 0.0)
            p.fillRect(QRectF(bar.left() + 1.0, y,
                              bar.width() - 1.0, std::max<qreal>(1.0, zeroY - y)),
                       positive);
        else
            p.fillRect(QRectF(bar.left() + 1.0, zeroY,
                              bar.width() - 1.0, std::max<qreal>(1.0, y - zeroY)),
                       negative);

        p.setPen(QPen(correlation_ < 0.0 ? negative.lighter(140) : positiveBright, 2.0));
        p.drawLine(QPointF(bar.left() - 3.0, y), QPointF(bar.right() + 3.0, y));

        QFont f = font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
        p.setFont(f);
        p.setPen(text);
        p.drawText(QRectF(full.left(), full.top(), full.width(), labelHeight),
                   Qt::AlignCenter, QStringLiteral("+1"));
        p.drawText(QRectF(full.left(), zeroY - 8.0, full.width(), 16.0),
                   Qt::AlignCenter, QStringLiteral("0"));
        p.drawText(QRectF(full.left(), bar.bottom(), full.width(), labelHeight),
                   Qt::AlignCenter, QStringLiteral("-1"));
    }

private:
    double correlation_ = 1.0;
};

PcmAudioScopeWidget::PcmAudioScopeWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(0, 0);
    setStyleSheet(QStringLiteral("background: rgb(5, 5, 5); color: rgb(238, 238, 235);"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    // No title strip: the black instrument surface starts immediately.
    auto* scopeRow = new QHBoxLayout();
    // One continuous black instrument surface.  Do not use layout spacing
    // here: on some Windows/Qt styles that exposed the parent window colour
    // as a bright vertical divider.  An explicit black spacer gives the phase
    // meter breathing room without creating a separate-looking pane.
    scopeRow->setContentsMargins(0, 0, 0, 0);
    scopeRow->setSpacing(0);

    phaseDisplay_ = new StereoPhaseDisplay(this);
    scopeRow->addWidget(phaseDisplay_, 1);

    auto* phaseGap = new QWidget(this);
    phaseGap->setFixedWidth(14);
    phaseGap->setStyleSheet(QStringLiteral("background: rgb(5,5,5); border: none;"));
    scopeRow->addWidget(phaseGap, 0);

    correlationMeter_ = new StereoCorrelationMeter(this);
    scopeRow->addWidget(correlationMeter_, 0);
    layout->addLayout(scopeRow, 1);

    auto* mudtwPanel = new QWidget(this);
    mudtwPanel->setObjectName(QStringLiteral("mudtwPanel"));
    mudtwPanel->setStyleSheet(QStringLiteral(
        "QWidget#mudtwPanel { background: rgb(5,5,5); border: none; }"));
    auto* mudtwLayout = new QVBoxLayout(mudtwPanel);
    // V0.9.2: the MUDTW area is a real, reserved layout block.  The badge is
    // no longer an overlaid child at y=3; that could be clipped by the scope
    // row when the viewport became short.  Keeping every item inside this
    // layout guarantees that the scope and MUDTW rectangles never overlap.
    mudtwLayout->setContentsMargins(12, 8, 12, 8);
    mudtwLayout->setSpacing(1);

    auto* mudtwLogo = new QLabel(QStringLiteral("MUDTW"), mudtwPanel);
    mudtwLogo->setObjectName(QStringLiteral("mudtwLogo"));
    QFont mudtwFont(QStringLiteral("Arial"));
    mudtwFont.setBold(true);
    mudtwFont.setStretch(QFont::Expanded);
    mudtwFont.setPointSize(8);
    mudtwLogo->setFont(mudtwFont);
    mudtwLogo->setStyleSheet(QStringLiteral(
        "color: rgb(205,205,200); background: transparent; border: none;"));
    mudtwLogo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mudtwLogo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mudtwLogo->setFixedHeight(16);

    leftMeter_ = new PcmAudioMeter(mudtwPanel);
    scale_ = new PcmAudioScale(mudtwPanel);
    rightMeter_ = new PcmAudioMeter(mudtwPanel);
    mudtwLayout->addWidget(mudtwLogo, 0);
    mudtwLayout->addWidget(leftMeter_, 0);
    mudtwLayout->addWidget(scale_, 0);
    mudtwLayout->addWidget(rightMeter_, 0);

    layout->addWidget(mudtwPanel, 0);
}


void PcmAudioScopeWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void PcmAudioScopeWidget::setExpandedLayout(bool expanded)
{
    if (expandedLayout_ == expanded)
        return;

    expandedLayout_ = expanded;
    updateResponsiveLayout();
}

void PcmAudioScopeWidget::updateResponsiveLayout()
{
    // V0.9.2: the MUDTW strip is always a separately reserved block.  In the
    // double-click/maximized Audio Scope view it deliberately gets a little
    // more height, so the MUDTW badge can stay visible and the meter can keep
    // the calmer PCM-Encoder proportions.  The phase scope simply uses the
    // remaining height; neither region can fold underneath the other.
    QWidget* mudtwPanel = findChild<QWidget*>(QStringLiteral("mudtwPanel"));
    if (mudtwPanel == nullptr)
        return;

    mudtwPanel->setVisible(true);

    const int availableHeight = std::max(0, height() - 8); // outer margins
    const double panelFraction = expandedLayout_ ? 0.24 : 0.20;
    const int desiredHeight = static_cast<int>(
        std::lround(static_cast<double>(availableHeight) * panelFraction));
    const int minPanelHeight = expandedLayout_ ? 112 : 48;
    // V0.9.3: do not pin the luxe MUDTW strip to a fixed pixel ceiling.
    // It must continue growing when the double-click/F11 viewport grows.
    const int maxPanelHeight = expandedLayout_
        ? std::max(minPanelHeight, availableHeight / 3)
        : 230;
    const int panelHeight = std::clamp(desiredHeight, minPanelHeight, maxPanelHeight);
    mudtwPanel->setMinimumHeight(panelHeight);
    mudtwPanel->setMaximumHeight(panelHeight);

    QLabel* mudtwLogo = mudtwPanel->findChild<QLabel*>(QStringLiteral("mudtwLogo"));
    // A double-click/maximized instrument always has enough real estate for
    // its badge; only the compact matrix presentation may drop the text.
    const bool showLogo = expandedLayout_ || (panelHeight >= 86 && width() >= 320);
    const int logoHeight = showLogo ? 16 : 0;
    if (mudtwLogo != nullptr)
    {
        mudtwLogo->setVisible(showLogo);
        mudtwLogo->setFixedHeight(logoHeight);
    }

    // Keep genuine black breathing room above and below the light bars.  The
    // expanded view gets slightly more air, matching the PCM Encoder meter.
    const double marginFraction = expandedLayout_ ? 0.10 : 0.08;
    const int outerY = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(panelHeight) * marginFraction)),
        expandedLayout_ ? 8 : 2,
        expandedLayout_ ? 30 : 24);
    if (QLayout* mudtwLayout = mudtwPanel->layout())
        mudtwLayout->setContentsMargins(12, outerY, 12, outerY);

    const int spacingBudget = 3;
    const int innerHeight = std::max(18,
        panelHeight - 2 * outerY - logoHeight - spacingBudget);
    const double scaleFraction = expandedLayout_ ? 0.42 : 0.40;
    const int scaleHeight = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(innerHeight) * scaleFraction)),
        18,
        expandedLayout_ ? 82 : 68);
    const int metersBudget = std::max(16, innerHeight - scaleHeight);
    const int meterHeight = std::clamp(
        metersBudget / 2,
        8,
        expandedLayout_ ? 42 : 40);

    if (leftMeter_ != nullptr)
        leftMeter_->setFixedHeight(meterHeight);
    if (rightMeter_ != nullptr)
        rightMeter_->setFixedHeight(meterHeight);
    if (scale_ != nullptr)
        scale_->setFixedHeight(scaleHeight);

    // V0.9.3: let the vertical phase/correlation meter use the horizontal
    // room that is actually available.  A fixed 64 px widget with a fixed
    // 12 px bar looked like a hairline in large/double-click/F11 views.
    if (correlationMeter_ != nullptr)
    {
        const double phaseFraction = expandedLayout_ ? 0.075 : 0.065;
        const int minPhaseWidth = expandedLayout_ ? 76 : 58;
        const int maxPhaseWidth = expandedLayout_ ? 118 : 86;
        const int phaseWidth = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(width()) * phaseFraction)),
            minPhaseWidth,
            maxPhaseWidth);
        correlationMeter_->setFixedWidth(phaseWidth);
    }
}

void PcmAudioScopeWidget::setAudioLevels(float leftPeak, float rightPeak)
{
    if (leftMeter_ != nullptr)
        leftMeter_->setInputPeak(leftPeak);
    if (rightMeter_ != nullptr)
        rightMeter_->setInputPeak(rightPeak);
}

void PcmAudioScopeWidget::setStereoPcm16(const QByteArray& stereoPcm16)
{
    if (phaseDisplay_ != nullptr)
        phaseDisplay_->setStereoPcm16(stereoPcm16);
    if (correlationMeter_ != nullptr)
        correlationMeter_->setStereoPcm16(stereoPcm16);
}

void PcmAudioScopeWidget::setColorized(bool enabled)
{
    if (phaseDisplay_ != nullptr)
        phaseDisplay_->setColorized(enabled);
}
