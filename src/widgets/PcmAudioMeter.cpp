#include "PcmAudioMeter.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QTimer>

namespace
{
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 0.0;
constexpr double kDangerDb = -1.0;
constexpr int kTinySegmentCount = 51;
constexpr int kCompactSegmentCount = 101;
constexpr int kFullSegmentCount = 201;

// Full 201 segments need enough horizontal pixels to remain visibly discrete.
// Below this threshold OpenScope deliberately uses half the segment density.
constexpr double kCompactMeterTrackWidth = 360.0;
constexpr double kFullMeterTrackWidth = 900.0;

constexpr double kDecayDbPerSecond = 60.0;
constexpr double kPeakHoldSeconds = 1.00;
constexpr double kPeakHoldDecayDbPerSecond = 24.0;

constexpr int kOuterMarginX = 6;
constexpr int kOuterMarginY = 2;
// Keep segment weight visually stable across compact, windowed and F11 views.
// Use a fraction of the available pitch instead of a fixed pixel gap: 101
// segments stay readable in quad mode while 201 segments do not turn into
// hairlines on a wide/fullscreen meter.
constexpr double kSegmentFillFraction = 0.48;
constexpr double kFullscreenSegmentFillFraction = 0.70;
constexpr int kMinSegmentWidth = 1;
constexpr int kMaxSegmentWidth = 4;
constexpr int kFullscreenMaxSegmentWidth = 6;

double dbToUnit(double db)
{
    // Same RTW-like law as OpenScopePCMEncoder. -20 dB is exactly halfway.
    constexpr std::array<double, 10> dbPoints{
        -60.0, -40.0, -30.0, -20.0, -15.0,
        -12.0,  -9.0,  -6.0,  -3.0,   0.0
    };
    constexpr std::array<double, 10> unitPoints{
         0.00,  0.17,  0.32,  0.50,  0.61,
         0.68,  0.75,  0.83,  0.91,  1.00
    };

    db = std::clamp(db, kMinDb, kMaxDb);
    for (std::size_t i = 1; i < dbPoints.size(); ++i)
    {
        if (db <= dbPoints[i])
        {
            const double spanDb = dbPoints[i] - dbPoints[i - 1];
            const double t = spanDb > 0.0
                ? (db - dbPoints[i - 1]) / spanDb
                : 0.0;
            return unitPoints[i - 1] +
                t * (unitPoints[i] - unitPoints[i - 1]);
        }
    }
    return 1.0;
}

QRectF meterTrackRect(const QWidget* widget)
{
    return QRectF(widget->rect()).adjusted(
        kOuterMarginX, kOuterMarginY, -kOuterMarginX, -kOuterMarginY);
}

int segmentCountForTrack(const QRectF& track)
{
    if (track.width() >= kFullMeterTrackWidth)
        return kFullSegmentCount;
    if (track.width() >= kCompactMeterTrackWidth)
        return kCompactSegmentCount;
    return kTinySegmentCount;
}

QColor inactiveSegment() { return QColor(22, 17, 12); }
QColor normalSegment() { return QColor(238, 132, 28); }
QColor dangerSegment() { return QColor(220, 28, 28); }

int dbToSegmentIndex(double db, int segmentCount)
{
    return std::clamp(
        static_cast<int>(std::lround(
            dbToUnit(db) * static_cast<double>(segmentCount - 1))),
        0,
        segmentCount - 1);
}

QRect segmentRectForIndex(
    const QRectF& track,
    int index,
    int top,
    int height,
    int segmentCount,
    bool fullscreen)
{
    const double usableWidth = track.width() - 2.0;
    const double pitch = usableWidth / static_cast<double>(segmentCount);

    // Keep the LED bars visually stable while the viewport is resized.
    // Previously a 101->201 segment-density switch could make the bars jump
    // from chunky to hairline-thin.  Cap the painted LED width and centre it
    // in its pitch instead; density can change without changing the visual
    // weight of an individual segment.
    // Always preserve visible black between neighbouring LEDs.  At narrow
    // widths the segment count is reduced before the bars are allowed to merge.
    const double fillFraction = fullscreen
        ? kFullscreenSegmentFillFraction
        : kSegmentFillFraction;
    const int maxSegmentWidth = fullscreen
        ? kFullscreenMaxSegmentWidth
        : kMaxSegmentWidth;

    const int maxWidthForGap = std::max(1, static_cast<int>(std::floor(pitch)) - 1);
    const int segWidth = std::clamp(
        static_cast<int>(std::lround(pitch * fillFraction)),
        kMinSegmentWidth,
        std::min(maxSegmentWidth, maxWidthForGap));
    const double centerX = track.left() + 1.0 +
        (static_cast<double>(index) + 0.5) * pitch;
    const int x0 = static_cast<int>(std::lround(centerX - 0.5 * segWidth));
    return QRect(x0, top, segWidth, height);
}

double segmentCenterX(const QRectF& track, int index, int segmentCount)
{
    const double usableWidth = track.width() - 2.0;
    const double pitch = usableWidth / static_cast<double>(segmentCount);
    return track.left() + 1.0 +
        (static_cast<double>(index) + 0.5) * pitch;
}

double scaleAnchorX(const QRectF& track, double db, int segmentCount)
{
    return segmentCenterX(track, dbToSegmentIndex(db, segmentCount), segmentCount);
}

double labelLeftForAnchor(
    const QFontMetrics& fm,
    const QString& label,
    int db,
    double anchorX)
{
    if (db < 0)
    {
        const int signWidth = fm.horizontalAdvance(QStringLiteral("-"));
        const int magnitudeWidth = fm.horizontalAdvance(QString::number(-db));
        return anchorX - static_cast<double>(signWidth) -
            static_cast<double>(magnitudeWidth) * 0.5;
    }
    return anchorX - static_cast<double>(fm.horizontalAdvance(label)) * 0.5;
}
}

PcmAudioMeter::PcmAudioMeter(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(8);

    animationTimer_ = new QTimer(this);
    animationTimer_->setInterval(16);
    connect(animationTimer_, &QTimer::timeout,
            this, &PcmAudioMeter::animateBallistics);
    elapsed_.start();
    animationTimer_->start();
}

void PcmAudioMeter::setInputPeak(float linearPeak)
{
    targetDb_ = linearToDb(static_cast<double>(linearPeak));
    if (targetDb_ > displayedDb_)
        displayedDb_ = targetDb_;

    if (targetDb_ >= peakHoldDb_)
    {
        peakHoldDb_ = targetDb_;
        peakHoldSecondsRemaining_ = kPeakHoldSeconds;
    }
    update();
}

QSize PcmAudioMeter::sizeHint() const { return QSize(520, 24); }
QSize PcmAudioMeter::minimumSizeHint() const { return QSize(120, 8); }

void PcmAudioMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(5, 5, 5));

    const QRectF track = meterTrackRect(this);
    if (track.width() <= 20.0 || track.height() <= 4.0)
        return;

    const int segmentCount = segmentCountForTrack(track);

    p.setPen(QPen(QColor(5, 5, 5), 1.0));
    p.setBrush(QColor(8, 8, 8));
    p.drawRect(track);

    const int activeSegments = std::clamp(
        static_cast<int>(std::floor(
            dbToUnit(displayedDb_) * static_cast<double>(segmentCount) + 0.5)),
        0,
        segmentCount);

    // Use most of the available vertical track.  Delta71 kept a fixed 4 px
    // inset on both sides, which reduced a compact 20 px meter to a nearly
    // dotted line.  Scale the inset with height and clamp it so the LEDs grow
    // with the MUDTW panel while retaining a little black breathing room.
    const bool fullscreen = window() != nullptr && window()->isFullScreen();
    const double segmentInsetFraction = fullscreen ? 0.20 : 0.24;
    const double segmentInsetY = std::clamp(
        track.height() * segmentInsetFraction, 4.0, fullscreen ? 14.0 : 12.0);
    const int segTop = static_cast<int>(std::round(track.top() + segmentInsetY));
    const int segHeight = std::max(
        2,
        static_cast<int>(std::round(track.height() - 2.0 * segmentInsetY)));

    for (int i = 0; i < segmentCount; ++i)
    {
        const QRect segment = segmentRectForIndex(
            track, i, segTop, segHeight, segmentCount, fullscreen);
        const double segmentUnit =
            static_cast<double>(i + 1) / static_cast<double>(segmentCount);

        QColor fill = inactiveSegment();
        if (i < activeSegments)
            fill = segmentUnit >= dbToUnit(kDangerDb)
                ? dangerSegment()
                : normalSegment();
        p.fillRect(segment, fill);
    }

    if (peakHoldDb_ > kMinDb + 0.01)
    {
        const int holdIndex = dbToSegmentIndex(peakHoldDb_, segmentCount);
        const QRect holdSegment = segmentRectForIndex(
            track, holdIndex, segTop, segHeight, segmentCount, fullscreen);
        p.fillRect(
            holdSegment,
            peakHoldDb_ >= kDangerDb
                ? QColor(255, 70, 55)
                : QColor(238, 132, 28));
    }

    const double dangerX = scaleAnchorX(track, kDangerDb, segmentCount);
    p.setPen(QPen(QColor(185, 25, 25), 1));
    p.drawLine(
        QPointF(dangerX, track.top() + 1.0),
        QPointF(dangerX, track.bottom() - 1.0));
}

void PcmAudioMeter::animateBallistics()
{
    const qint64 elapsedMs = elapsed_.restart();
    if (elapsedMs <= 0)
        return;

    const double dt = static_cast<double>(elapsedMs) / 1000.0;
    bool changed = false;

    if (displayedDb_ > targetDb_)
    {
        displayedDb_ = std::max(
            targetDb_, displayedDb_ - kDecayDbPerSecond * dt);
        changed = true;
    }

    if (peakHoldSecondsRemaining_ > 0.0)
    {
        peakHoldSecondsRemaining_ =
            std::max(0.0, peakHoldSecondsRemaining_ - dt);
    }
    else if (peakHoldDb_ > displayedDb_)
    {
        peakHoldDb_ = std::max(
            displayedDb_, peakHoldDb_ - kPeakHoldDecayDbPerSecond * dt);
        changed = true;
    }
    else
    {
        peakHoldDb_ = displayedDb_;
    }

    if (changed)
        update();
}

double PcmAudioMeter::linearToDb(double linear)
{
    if (!std::isfinite(linear) || linear <= 0.0)
        return kMinDb;
    return std::clamp(20.0 * std::log10(linear), kMinDb, kMaxDb);
}

PcmAudioScale::PcmAudioScale(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(18);
}

QSize PcmAudioScale::sizeHint() const { return QSize(520, 38); }
QSize PcmAudioScale::minimumSizeHint() const { return QSize(120, 18); }

void PcmAudioScale::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.fillRect(rect(), QColor(5, 5, 5));

    const QRectF track = meterTrackRect(this);
    if (track.width() <= 20.0)
        return;

    const int segmentCount = segmentCountForTrack(track);
    constexpr std::array<int, 10> majorDb{
        -60, -40, -30, -20, -15, -12, -9, -6, -3, 0
    };
    constexpr std::array<int, 9> minorDb{
        -50, -35, -25, -18, -14, -10, -8, -5, -1
    };

    const QColor text(232, 232, 228);
    const QColor tick(210, 210, 205);
    const double h = static_cast<double>(height());
    const double topPad = std::clamp(h * 0.04, 2.0, 5.0);
    const double bottomPad = std::clamp(h * 0.04, 2.0, 5.0);
    const double tickLen = std::clamp(h * 0.18, 5.0, 10.0);
    const double minorInset = std::clamp(tickLen * 0.45, 2.0, 5.0);

    QFont f = p.font();
    f.setPointSizeF(std::clamp(h * 0.25, 6.0, 12.0));
    f.setWeight(QFont::Bold);
    p.setFont(f);
    const QFontMetrics fm(f);

    const double upperTickTop = topPad;
    const double upperTickBottom = upperTickTop + tickLen;
    const double lowerTickBottom = h - bottomPad;
    const double lowerTickTop = lowerTickBottom - tickLen;

    p.setPen(QPen(tick, 1));
    for (const int db : majorDb)
    {
        const double x = scaleAnchorX(track, static_cast<double>(db), segmentCount);
        p.drawLine(QPointF(x, upperTickTop), QPointF(x, upperTickBottom));
        p.drawLine(QPointF(x, lowerTickTop), QPointF(x, lowerTickBottom));
    }

    p.setPen(QPen(QColor(145, 145, 140), 1));
    for (const int db : minorDb)
    {
        const double x = scaleAnchorX(track, static_cast<double>(db), segmentCount);
        p.drawLine(
            QPointF(x, upperTickTop + minorInset),
            QPointF(x, upperTickBottom));
        p.drawLine(
            QPointF(x, lowerTickTop),
            QPointF(x, lowerTickBottom - minorInset));
    }

    const double textBandTop = upperTickBottom + 1.0;
    const double textBandBottom = lowerTickTop - 1.0;
    p.setPen(text);
    // On a narrow quad pane keep the scale legible by thinning labels rather
    // than allowing them to collide.  Major ticks remain at every anchor.
    auto drawMajorLabel = [this](int db)
    {
        if (width() >= 420)
            return true;
        if (width() >= 300)
            return db == -60 || db == -40 || db == -20 || db == -12 || db == -6 || db == 0;
        return db == -60 || db == -20 || db == -6 || db == 0;
    };
    for (const int db : majorDb)
    {
        if (!drawMajorLabel(db))
            continue;
        const double x = scaleAnchorX(track, static_cast<double>(db), segmentCount);
        const QString label = QString::number(db);
        const int labelWidth = fm.horizontalAdvance(label);
        const double labelX = labelLeftForAnchor(fm, label, db, x);
        p.drawText(
            QRectF(labelX, textBandTop,
                   static_cast<double>(labelWidth),
                   std::max(1.0, textBandBottom - textBandTop)),
            Qt::AlignLeft | Qt::AlignVCenter,
            label);
    }

    const double dangerX = scaleAnchorX(track, kDangerDb, segmentCount);
    p.setPen(QPen(QColor(205, 30, 30), 2));
    p.drawLine(QPointF(dangerX, upperTickTop), QPointF(dangerX, upperTickBottom));
    p.drawLine(QPointF(dangerX, lowerTickTop), QPointF(dangerX, lowerTickBottom));
}
