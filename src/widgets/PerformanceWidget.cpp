#include "PerformanceWidget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSlider>
#include <QStringList>
#include <QToolTip>

namespace
{
    constexpr double kMaximumUs = 80000.0;
    constexpr double kMinimumTimelineSpanUs = 5000.0;
    constexpr int kToolbarHeight = 28;
    constexpr int kTimelineScrollHeight = 18;

    double gTimelineStartUs = 0.0;
    double gTimelineSpanUs = kMaximumUs;
    constexpr double kField1DeadlineUs = 40000.0;
    constexpr double kField2DeadlineUs = 60000.0;
    constexpr double kTimingToleranceUs = 2000.0;

    constexpr int kBarHeight = 20;
    constexpr int kRowSpacing = 8;
    constexpr int kMargin = 12;
    constexpr int kScaleHeight = 26;

    enum class RowKind
    {
        FieldTiming,
        TimingDiagnostics,
        Field1Ready,
        Field2Ready,
        DisplayWorker0,
        DisplayWorker1,
        ScreenVideo,
        WaveformWorker,
        WaveformScreen,
        WaveformSpout,
        VectorscopeWorker,
        VectorscopeScreen,
        VectorscopeSpout,
        DisplayCompose
    };

    struct Row
    {
        const char* label;
        RowKind kind;
    };

    constexpr std::array<Row, 14> kRows =
    {{
        { "Field timing", RowKind::FieldTiming },
        { "Timing diagnostics [events]", RowKind::TimingDiagnostics },
        { "Field 1 ready @ 40 ms", RowKind::Field1Ready },
        { "Field 2 ready @ 60 ms", RowKind::Field2Ready },
        { "Video worker 1 [timeline]", RowKind::DisplayWorker0 },
        { "Video worker 2 [timeline]", RowKind::DisplayWorker1 },
        { "Screen Frame Cost [wallclock]", RowKind::ScreenVideo },
        { "Waveform worker (Screen + Spout) [timeline]", RowKind::WaveformWorker },
        { "  -> Screen waveform [timeline]", RowKind::WaveformScreen },
        { "  -> Spout waveform [timeline]", RowKind::WaveformSpout },
        { "Vectorscope worker (Screen + Spout) [timeline]", RowKind::VectorscopeWorker },
        { "  -> Screen vectorscope [cost]", RowKind::VectorscopeScreen },
        { "  -> Spout vectorscope [cost]", RowKind::VectorscopeSpout },
        { "Display compose CPU sum [work]", RowKind::DisplayCompose }
    }};

    int xForUs(
        const QRect& barRect,
        double us)
    {
        const double normalized =
            (us - gTimelineStartUs) /
            std::max(gTimelineSpanUs, 1.0);

        return
            barRect.left() +
            static_cast<int>(
                std::lround(
                    normalized *
                    static_cast<double>(
                        barRect.width())));
    }

    bool isUsVisible(double us)
    {
        return
            us >= gTimelineStartUs &&
            us <= gTimelineStartUs + gTimelineSpanUs;
    }

    void drawDeadlineGuides(
        QPainter& painter,
        const QRect& barRect)
    {
        QPen pen(QColor(75, 75, 75));
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);

        for (const double us :
            { kField1DeadlineUs, kField2DeadlineUs })
        {
            if (!isUsVisible(us))
            {
                continue;
            }

            const int x =
                xForUs(barRect, us);

            painter.drawLine(
                x,
                barRect.top(),
                x,
                barRect.bottom());
        }
    }

    void fillTimingBackground(
        QPainter& painter,
        const QRect& barRect,
        double greenEndUs,
        double yellowEndUs)
    {
        const int greenEnd =
            std::clamp(
                xForUs(barRect, greenEndUs),
                barRect.left(),
                barRect.right() + 1);

        const int yellowEnd =
            std::clamp(
                xForUs(barRect, yellowEndUs),
                barRect.left(),
                barRect.right() + 1);

        painter.fillRect(
            QRect(
                barRect.left(),
                barRect.top(),
                std::max(greenEnd - barRect.left(), 0),
                barRect.height()),
            QColor(30, 125, 48));

        painter.fillRect(
            QRect(
                greenEnd,
                barRect.top(),
                std::max(yellowEnd - greenEnd, 0),
                barRect.height()),
            QColor(155, 132, 35));

        painter.fillRect(
            QRect(
                yellowEnd,
                barRect.top(),
                std::max(barRect.right() + 1 - yellowEnd, 0),
                barRect.height()),
            QColor(145, 38, 38));
    }

    void fillFieldTimingBackground(
        QPainter& painter,
        const QRect& barRect)
    {
        painter.fillRect(
            barRect,
            QColor(28, 28, 28));

        for (const double targetUs :
            { kField1DeadlineUs, kField2DeadlineUs })
        {
            const double bandStartUs =
                targetUs - kTimingToleranceUs;
            const double bandEndUs =
                targetUs + kTimingToleranceUs;

            if (bandEndUs < gTimelineStartUs ||
                bandStartUs >
                    gTimelineStartUs + gTimelineSpanUs)
            {
                continue;
            }

            const int left =
                std::clamp(
                    xForUs(barRect, bandStartUs),
                    barRect.left(),
                    barRect.right() + 1);

            const int right =
                std::clamp(
                    xForUs(barRect, bandEndUs),
                    barRect.left(),
                    barRect.right() + 1);

            painter.fillRect(
                QRect(
                    left,
                    barRect.top(),
                    std::max(right - left, 1),
                    barRect.height()),
                QColor(30, 125, 48));

            if (isUsVisible(targetUs))
            {
                painter.setPen(
                    QColor(150, 150, 150));

                const int x =
                    xForUs(barRect, targetUs);

                painter.drawLine(
                    x,
                    barRect.top(),
                    x,
                    barRect.bottom());
            }
        }
    }

    void drawFieldStatusMarker(
        QPainter& painter,
        const QRect& barRect,
        double valueUs,
        double targetUs)
    {
        const bool inRange =
            std::abs(valueUs - targetUs) <=
            kTimingToleranceUs;

        if (!isUsVisible(targetUs))
        {
            return;
        }

        const int x =
            xForUs(barRect, targetUs);

        const QRect markerRect(
            x - 7,
            barRect.center().y() - 7,
            14,
            14);

        QPen pen(
            inRange
                ? QColor(95, 215, 120)
                : QColor(235, 95, 95));

        pen.setWidth(2);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);

        if (inRange)
        {
            painter.drawLine(
                markerRect.left() + 2,
                markerRect.center().y(),
                markerRect.left() + 6,
                markerRect.bottom() - 2);

            painter.drawLine(
                markerRect.left() + 6,
                markerRect.bottom() - 2,
                markerRect.right() - 1,
                markerRect.top() + 2);
        }
        else
        {
            painter.drawLine(
                markerRect.topLeft() + QPoint(2, 2),
                markerRect.bottomRight() - QPoint(2, 2));

            painter.drawLine(
                markerRect.topRight() + QPoint(-2, 2),
                markerRect.bottomLeft() + QPoint(2, -2));
        }
    }

    void drawSegment(
        QPainter& painter,
        const QRect& barRect,
        double startUs,
        double durationUs,
        const QColor& color,
        const QString& label,
        bool outlineOnly = false)
    {
        if (durationUs <= 0.0)
        {
            return;
        }

        const double endUs =
            startUs + durationUs;

        if (endUs < gTimelineStartUs ||
            startUs >
                gTimelineStartUs + gTimelineSpanUs)
        {
            return;
        }

        const int left =
            std::clamp(
                xForUs(barRect, startUs),
                barRect.left(),
                barRect.right());

        const int right =
            std::clamp(
                xForUs(barRect, endUs),
                barRect.left(),
                barRect.right() + 1);

        const QRect segmentRect(
            left,
            barRect.top() + 1,
            std::max(right - left, 1),
            std::max(barRect.height() - 2, 1));

        if (!outlineOnly)
        {
            painter.fillRect(
                segmentRect,
                color);
        }

        QPen pen(
            outlineOnly
                ? QColor(175, 175, 175)
                : QColor(35, 35, 35));

        if (outlineOnly)
        {
            pen.setStyle(Qt::DashLine);
        }

        painter.setPen(pen);
        painter.drawRect(
            segmentRect.adjusted(0, 0, -1, -1));

        if (!outlineOnly &&
            segmentRect.width() >= 10)
        {
            painter.setPen(Qt::black);
            painter.drawText(
                segmentRect,
                Qt::AlignCenter,
                label);
        }
    }

    void drawPriorityUnderline(
        QPainter& painter,
        const QRect& barRect,
        double startUs,
        double durationUs)
    {
        if (durationUs <= 0.0)
        {
            return;
        }

        const double endUs = startUs + durationUs;
        if (endUs < gTimelineStartUs ||
            startUs > gTimelineStartUs + gTimelineSpanUs)
        {
            return;
        }

        const int left = std::clamp(
            xForUs(barRect, startUs),
            barRect.left(),
            barRect.right());
        const int right = std::clamp(
            xForUs(barRect, endUs),
            barRect.left(),
            barRect.right() + 1);

        QPen pen(QColor(235, 45, 45));
        pen.setWidth(2);
        painter.setPen(pen);
        const int y = barRect.bottom() - 2;
        painter.drawLine(left, y, std::max(right - 1, left), y);
    }

    const char* displayPhaseText(char phase)
    {
        switch (phase)
        {
        case 'F': return "F";
        case 'N': return "N";
        case 'D': return "D";
        case '1': return "C1";
        case 'A': return "S1";
        case '2': return "C2";
        case 'B': return "S2";
        default:  return "?";
        }
    }

    const char* displayPhaseName(char phase)
    {
        switch (phase)
        {
        case 'F': return "Frequency compensation";
        case 'N': return "Noise reduction";
        case 'D': return "Deinterlace";
        case '1': return "RGB convert field 1";
        case 'A': return "Spout buffer field 1";
        case '2': return "RGB convert field 2";
        case 'B': return "Spout buffer field 2";
        default:  return "Unknown display phase";
        }
    }

    QColor displayPhaseColor(char phase)
    {
        switch (phase)
        {
        case 'F': return QColor(235, 215, 180);
        case 'N': return QColor(185, 225, 205);
        case 'D': return QColor(185, 215, 235);
        case '1': return QColor(215, 200, 235);
        case 'A': return QColor(205, 225, 185);
        case '2': return QColor(205, 190, 230);
        case 'B': return QColor(190, 215, 170);
        default:  return QColor(205, 205, 205);
        }
    }

    const char* waveformPhaseText(char phase)
    {
        switch (phase)
        {
        case 'F': return "F";
        case 'U': return "U";
        case 'T': return "T";
        case 'K': return "K";
        case 'E': return "E";
        case 'e': return "e";
        case 'H': return "H";
        case 'h': return "h";
        case 's': return "s";
        case 'g': return "g";
        case 'y': return "y";
        case 'A': return "A";
        case 'L': return "L";
        case 'J': return "J";
        case 'R':
        case 'r': return "R";
        case 'Z': return "Z";
        case 'P': return "P";
        case 'B': return "B";
        case 'G': return "G";
        case 'Q': return "Q";
        case 'C': return "C";
        case 'O': return "O";
        case 'X':
        case 'x': return "X";
        case 'W': return "W";
        default:  return "?";
        }
    }

    const char* waveformPhaseName(char phase)
    {
        switch (phase)
        {
        case 'F': return "Frequency compensation";
        case 'U': return "Luma upsample";
        case 'T': return "Trace preparation";
        case 'K': return "Packet classify / MUD sieve";
        case 'E': return "Energy target allocate / resize";
        case 'e': return "Energy target clear / reset";
        case 'H': return "Glow kernel preparation";
        case 'h': return "Glow sample preparation";
        case 's': return "Glow setup / bucket allocation";
        case 'g': return "Beam glow stamp / apply";
        case 'y': return "Resolve setup / scratch allocation";
        case 'A': return "AA / stitch setup";
        case 'L': return "Raster load / cost analysis";
        case 'J': return "Chunk partition / job dispatch";
        case 'R': return "Trace raster";
        case 'r': return "Spout trace raster";
        case 'Z': return "Raster zipper";
        case 'P': return "ScopePhor feedback / history";
        case 'B': return "Base image clear";
        case 'G': return "Graticule draw";
        case 'Q': return "Phosphor energy -> output image";
        case 'C': return "Chroma compose";
        case 'O': return "Line-info / overlay";
        case 'X': return "Resolve/output chunk";
        case 'x': return "Spout resolve/output chunk";
        case 'W': return "Assist rejoin wait";
        default:  return "Unknown waveform phase";
        }
    }

    QString waveformAssistDescription(
        char phase,
        std::uint32_t chunkIndex)
    {
        const char* base = waveformPhaseName(phase);
        if (phase == 'R' || phase == 'X' || phase == 'r' || phase == 'x' || phase == 'h')
        {
            return QStringLiteral("%1 chunk %2")
                .arg(QString::fromLatin1(base))
                .arg(chunkIndex);
        }

        return QString::fromLatin1(base);
    }

    QColor waveformPhaseColor(char phase)
    {
        switch (phase)
        {
        case 'F': return QColor(235, 215, 180);
        case 'U': return QColor(185, 215, 235);
        case 'T': return QColor(205, 215, 235);
        case 'K': return QColor(170, 205, 230);
        case 'E': return QColor(190, 210, 230);
        case 'e': return QColor(175, 205, 225);
        case 'H': return QColor(205, 220, 235);
        case 'h': return QColor(210, 210, 235);
        case 's': return QColor(200, 205, 230);
        case 'g': return QColor(220, 205, 235);
        case 'y': return QColor(205, 215, 230);
        case 'A': return QColor(195, 215, 230);
        case 'L': return QColor(185, 210, 225);
        case 'J': return QColor(175, 205, 220);
        case 'R':
        case 'r': return QColor(238, 210, 165);
        case 'Z': return QColor(205, 190, 230);
        case 'P': return QColor(185, 225, 205);
        case 'B': return QColor(215, 205, 230);
        case 'G': return QColor(205, 215, 235);
        case 'Q': return QColor(160, 215, 195);
        case 'C': return QColor(205, 225, 185);
        case 'O': return QColor(225, 205, 205);
        case 'X':
        case 'x': return QColor(190, 200, 235);
        case 'W': return QColor(235, 205, 150);
        default:  return QColor(195, 195, 195);
        }
    }

    const char* workerPhaseText(char phase)
    {
        switch (phase)
        {
        case 'S': return "S";
        case 'V': return "V";
        case 'M': return "M";
        case 'E': return "E";
        case 'A':
        case 'a': return "A";
        case 'P':
        case 'p': return "P";
        case 'C':
        case 'c': return "C";
        case 'O':
        case 'o': return "O";
        default:  return "?";
        }
    }

    const char* waveformWorkerPhaseName(char phase)
    {
        switch (phase)
        {
        case 'S': return "Screen waveform render";
        case 'V': return "Spout waveform render";
        case 'M': return "Waveform measurement / spectrum publish prep";
        case 'E': return "Qt image publish / emit";
        default:  return "Unknown waveform-worker phase";
        }
    }

    const char* vectorscopeWorkerPhaseName(char phase)
    {
        switch (phase)
        {
        case 'S': return "Screen vectorscope render + emit envelope";
        case 'V': return "Spout vectorscope render + emit envelope";
        case 'A': return "Screen analyzer / density";
        case 'P': return "Screen persistence / glow";
        case 'C': return "Screen compose";
        case 'O': return "Screen targets / overlay";
        case 'a': return "Spout analyzer / density";
        case 'p': return "Spout persistence / glow";
        case 'c': return "Spout compose";
        case 'o': return "Spout targets / overlay";
        default:  return "Unknown vectorscope-worker phase";
        }
    }

    QColor workerPhaseColor(char phase)
    {
        switch (phase)
        {
        case 'S': return QColor(205, 225, 235);
        case 'V': return QColor(225, 205, 235);
        case 'M': return QColor(235, 225, 185);
        case 'E': return QColor(205, 235, 225);
        case 'A':
        case 'a': return QColor(205, 225, 235);
        case 'P':
        case 'p': return QColor(185, 225, 205);
        case 'C':
        case 'c': return QColor(205, 225, 185);
        case 'O':
        case 'o': return QColor(225, 205, 205);
        default:  return QColor(195, 195, 195);
        }
    }

    void drawDisplayTimeline(
        QPainter& painter,
        const QRect& barRect,
        const DisplayPhaseTimelineSnapshot& timeline,
        bool includeSecondField)
    {
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < DisplayPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.phase);

            if (phase == '^')
            {
                continue;
            }

            if (!includeSecondField &&
                (phase == '2' || phase == 'B'))
            {
                continue;
            }

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                displayPhaseColor(phase),
                QString::fromLatin1(displayPhaseText(phase)));
        }
    }

    void drawAssistTimeline(
        QPainter& painter,
        const QRect& barRect,
        const WaveformAssistTimelineSnapshot& timeline)
    {
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.phase);

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                waveformPhaseColor(phase),
                QString::fromLatin1(waveformPhaseText(phase)));
        }
    }

    void drawWorkerTimeline(
        QPainter& painter,
        const QRect& barRect,
        const WorkerPhaseTimelineSnapshot& timeline,
        bool outlineScreenEnvelope = false)
    {
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.phase);

            if (phase == '^')
            {
                continue;
            }

            // S and V are parent wallclock envelopes, not measured work of
            // their own. On the waveform worker row keep them as context only:
            // unfilled dashed outlines. Their exact renderer child phases are
            // drawn on top below. Any time not covered by a real child phase
            // must remain visibly empty so instrumentation gaps stand out.
            const bool outlineOnly =
                outlineScreenEnvelope &&
                (phase == 'S' || phase == 'V');

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                workerPhaseColor(phase),
                QString::fromLatin1(workerPhaseText(phase)),
                outlineOnly);
        }
    }

    void drawPriorityStateTimeline(
        QPainter& painter,
        const QRect& barRect,
        const WorkerPhaseTimelineSnapshot& timeline)
    {
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            if (static_cast<char>(event.phase) != '^')
            {
                continue;
            }

            drawPriorityUnderline(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs));
        }
    }

    void drawFrequencyPriorityTimeline(
        QPainter& painter,
        const QRect& barRect,
        const WaveformAssistTimelineSnapshot& timeline)
    {
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            if (static_cast<char>(event.phase) != 'F')
            {
                continue;
            }

            drawPriorityUnderline(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs));
        }
    }

    void drawWaveformScreenTimeline(
        QPainter& painter,
        const QRect& barRect,
        const PerformanceSnapshot& snapshot)
    {
        // Screen waveform is a real capture-relative timeline.  Renderer R/X
        // entries are aggregate envelopes, so do not draw them here.  The
        // actual parallel R/X chunks are published by the assist timelines and
        // carry the worker that really executed each chunk.
        int glowWorkerCount = 0;
        const std::uint64_t currentAssistGeneration =
            snapshot.waveformWorkerAssist.generation;

        const auto hasGlowForCurrentGeneration =
            [&](const WaveformAssistTimelineSnapshot& assist)
            {
                if (currentAssistGeneration != 0u &&
                    assist.generation != currentAssistGeneration)
                {
                    return false;
                }

                for (std::uint32_t i = 0;
                    i < assist.count &&
                    i < WaveformAssistTimelineSnapshot::kCapacity;
                    ++i)
                {
                    if (static_cast<char>(assist.events[i].phase) == 'g')
                    {
                        return true;
                    }
                }

                return false;
            };

        glowWorkerCount += hasGlowForCurrentGeneration(snapshot.waveformWorkerAssist) ? 1 : 0;
        glowWorkerCount += hasGlowForCurrentGeneration(snapshot.displayWorker0Assist) ? 1 : 0;
        glowWorkerCount += hasGlowForCurrentGeneration(snapshot.displayWorker1Assist) ? 1 : 0;
        glowWorkerCount += hasGlowForCurrentGeneration(snapshot.vectorscopeWorkerAssist) ? 1 : 0;

        const auto& phases = snapshot.waveformScreenPhases;
        for (std::uint32_t i = 0;
            i < phases.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = phases.events[i];
            const char phase = static_cast<char>(event.label);

            if (phase == 'F' || phase == 'R' || phase == 'X')
            {
                continue;
            }

            const QString label =
                (phase == 'g')
                    ? QString(std::max(1, glowWorkerCount), QChar('g'))
                    : QString::fromLatin1(waveformPhaseText(phase));

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                waveformPhaseColor(phase),
                label);
        }

        // The Screen waveform row is a COMPOSITE timeline. Parallel assist
        // chunks must not be drawn individually here: that falsely suggests
        // serial execution. Draw one wallclock envelope per parallel phase.
        // The worker rows/details retain the individual chunks and ownership.
        const auto drawParallelEnvelope =
            [&](char wantedPhase)
            {
                bool found = false;
                std::uint64_t firstUs = 0;
                std::uint64_t lastUs = 0;
                int workerCount = 0;

                const std::uint64_t currentAssistGeneration =
                    snapshot.waveformWorkerAssist.generation;

                const auto include =
                    [&](const WaveformAssistTimelineSnapshot& assist)
                    {
                        if (currentAssistGeneration != 0u &&
                            assist.generation != currentAssistGeneration)
                        {
                            return;
                        }

                        bool workerUsed = false;
                        for (std::uint32_t i = 0;
                            i < assist.count &&
                            i < WaveformAssistTimelineSnapshot::kCapacity;
                            ++i)
                        {
                            const auto& event = assist.events[i];
                            if (static_cast<char>(event.phase) != wantedPhase)
                            {
                                continue;
                            }

                            workerUsed = true;
                            const std::uint64_t startUs = event.startUs;
                            const std::uint64_t endUs =
                                startUs + event.durationUs;
                            if (!found)
                            {
                                firstUs = startUs;
                                lastUs = endUs;
                                found = true;
                            }
                            else
                            {
                                firstUs = std::min(firstUs, startUs);
                                lastUs = std::max(lastUs, endUs);
                            }
                        }
                        if (workerUsed)
                        {
                            ++workerCount;
                        }
                    };

                include(snapshot.waveformWorkerAssist);
                include(snapshot.displayWorker0Assist);
                include(snapshot.displayWorker1Assist);
                include(snapshot.vectorscopeWorkerAssist);

                if (found && lastUs >= firstUs)
                {
                    const QString label(
                        std::max(1, workerCount),
                        QChar::fromLatin1(wantedPhase));
                    drawSegment(
                        painter,
                        barRect,
                        static_cast<double>(firstUs),
                        static_cast<double>(lastUs - firstUs),
                        waveformPhaseColor(wantedPhase),
                        label);
                }
            };

        drawParallelEnvelope('R');
        drawParallelEnvelope('X');
    }

    void drawWaveformSpoutTimeline(
        QPainter& painter,
        const QRect& barRect,
        const PerformanceSnapshot& snapshot)
    {
        const auto& rendererTimeline = snapshot.waveformVideoPhases;

        // Draw all serial Spout renderer phases except R/X.  R/X are aggregate
        // renderer envelopes once the shared worker queue is enabled; their
        // real wallclock shape comes from lower-case r/x assist jobs below.
        if (rendererTimeline.generation != 0u && rendererTimeline.count > 0u)
        {
            for (std::uint32_t i = 0;
                i < rendererTimeline.count &&
                i < WaveformPhaseTimelineSnapshot::kCapacity;
                ++i)
            {
                const auto& event = rendererTimeline.events[i];
                const char phase = static_cast<char>(event.label);
                if (phase == 'R' || phase == 'X')
                {
                    continue;
                }

                drawSegment(
                    painter,
                    barRect,
                    static_cast<double>(event.startUs),
                    static_cast<double>(event.durationUs),
                    waveformPhaseColor(phase),
                    QString::fromLatin1(waveformPhaseText(phase)));
            }

            const auto drawParallelEnvelope =
                [&](char assistPhase, char visiblePhase)
                {
                    bool found = false;
                    std::uint64_t firstUs = 0u;
                    std::uint64_t lastUs = 0u;
                    int workerCount = 0;
                    const auto include =
                        [&](const WaveformAssistTimelineSnapshot& assist)
                        {
                            bool used = false;
                            for (std::uint32_t i = 0;
                                i < assist.count &&
                                i < WaveformAssistTimelineSnapshot::kCapacity;
                                ++i)
                            {
                                const auto& event = assist.events[i];
                                if (static_cast<char>(event.phase) != assistPhase)
                                {
                                    continue;
                                }
                                used = true;
                                const std::uint64_t begin = event.startUs;
                                const std::uint64_t finish = begin + event.durationUs;
                                if (!found)
                                {
                                    firstUs = begin;
                                    lastUs = finish;
                                    found = true;
                                }
                                else
                                {
                                    firstUs = std::min(firstUs, begin);
                                    lastUs = std::max(lastUs, finish);
                                }
                            }
                            if (used)
                            {
                                ++workerCount;
                            }
                        };

                    include(snapshot.waveformWorkerAssist);
                    include(snapshot.displayWorker0Assist);
                    include(snapshot.displayWorker1Assist);
                    include(snapshot.vectorscopeWorkerAssist);

                    if (found && lastUs >= firstUs)
                    {
                        drawSegment(
                            painter,
                            barRect,
                            static_cast<double>(firstUs),
                            static_cast<double>(lastUs - firstUs),
                            waveformPhaseColor(visiblePhase),
                            QString(
                                std::max(1, workerCount),
                                QChar::fromLatin1(visiblePhase)));
                    }
                };

            drawParallelEnvelope('r', 'R');
            drawParallelEnvelope('x', 'X');
            return;
        }

        const auto& workerTimeline = snapshot.waveformWorkerPhases;
        for (std::uint32_t i = 0;
            i < workerTimeline.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = workerTimeline.events[i];
            if (static_cast<char>(event.phase) != 'V')
            {
                continue;
            }
            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                workerPhaseColor('V'),
                QStringLiteral("V"));
        }
    }

    void drawWaveformWorkerScreenPhases(
        QPainter& painter,
        const QRect& barRect,
        const WaveformPhaseTimelineSnapshot& timeline)
    {
        // These are the exact Screen-render sub-phases. They belong on the
        // Waveform worker chronology, not on a separate child timeline.
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.label);

            // X is only a render envelope. F here is the Waveform
            // worker's own one-line frequency compensation.
            if (phase == 'X' || phase == 'R')
            {
                continue;
            }

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                waveformPhaseColor(phase),
                QString::fromLatin1(waveformPhaseText(phase)));
        }
    }

    void drawWaveformWorkerSpoutPhases(
        QPainter& painter,
        const QRect& barRect,
        const WaveformPhaseTimelineSnapshot& timeline)
    {
        // These are the exact Spout-render sub-phases. They belong on the
        // same Waveform worker chronology as the outer V envelope.
        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.label);

            // Delta 58: Spout R/X are parallel assist envelopes now.  The
            // individual r/x chunks are drawn from the shared assist timeline.
            if (phase == 'R' || phase == 'X')
            {
                continue;
            }

            drawSegment(
                painter,
                barRect,
                static_cast<double>(event.startUs),
                static_cast<double>(event.durationUs),
                waveformPhaseColor(phase),
                QString::fromLatin1(waveformPhaseText(phase)));
        }
    }

    void drawPrerequisiteWait(
        QPainter& painter,
        const QRect& barRect,
        double endUs,
        const QString& label)
    {
        if (endUs <= 0.0 ||
            gTimelineStartUs >= endUs)
        {
            return;
        }

        if (gTimelineStartUs + gTimelineSpanUs <= 0.0)
        {
            return;
        }

        const int left =
            std::clamp(
                xForUs(barRect, 0.0),
                barRect.left(),
                barRect.right());

        const int right =
            std::clamp(
                xForUs(barRect, endUs),
                barRect.left(),
                barRect.right() + 1);

        if (right <= left)
        {
            return;
        }

        QRect waitRect(
            left,
            barRect.top() + 2,
            std::max(right - left, 1),
            std::max(barRect.height() - 4, 1));

        QPen waitPen(QColor(210, 190, 120));
        waitPen.setStyle(Qt::DashLine);
        waitPen.setWidth(1);

        painter.setPen(waitPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(
            waitRect.adjusted(0, 0, -1, -1));

        painter.setPen(QColor(220, 205, 155));
        painter.drawText(
            waitRect,
            Qt::AlignCenter,
            label);
    }

    void drawCostSegment(
        QPainter& painter,
        const QRect& barRect,
        double& cursorUs,
        const PerformanceMetricSnapshot& metric,
        const QColor& color,
        const QString& label)
    {
        const double durationUs =
            static_cast<double>(metric.latestUs);

        drawSegment(
            painter,
            barRect,
            cursorUs,
            durationUs,
            color,
            label);

        cursorUs += durationUs;
    }

    void drawWaveformCostBreakdown(
        QPainter& painter,
        const QRect& barRect,
        const PerformanceSnapshot& snapshot)
    {
        double cursorUs = 0.0;

        drawCostSegment(
            painter, barRect, cursorUs,
            snapshot.waveformVideoTrace,
            waveformPhaseColor('R'),
            QStringLiteral("R"));

        drawCostSegment(
            painter, barRect, cursorUs,
            snapshot.waveformVideoPersistence,
            waveformPhaseColor('P'),
            QStringLiteral("P*"));

        drawCostSegment(
            painter, barRect, cursorUs,
            snapshot.waveformVideoCompose,
            waveformPhaseColor('C'),
            QStringLiteral("C"));

        drawCostSegment(
            painter, barRect, cursorUs,
            snapshot.waveformVideoGlow,
            QColor(205, 205, 235),
            QStringLiteral("G"));

        drawCostSegment(
            painter, barRect, cursorUs,
            snapshot.waveformVideoOverlay,
            waveformPhaseColor('O'),
            QStringLiteral("O"));

        const double remainderUs =
            std::max(
                static_cast<double>(snapshot.waveformVideo.latestUs) -
                    cursorUs,
                0.0);

        if (remainderUs > 0.0)
        {
            drawSegment(
                painter,
                barRect,
                cursorUs,
                remainderUs,
                QColor(130, 130, 130),
                QStringLiteral("?"));
        }
    }

    void drawVectorscopeCostBreakdown(
        QPainter& painter,
        const QRect& barRect,
        const PerformanceMetricSnapshot& total,
        const PerformanceMetricSnapshot& analyzer,
        const PerformanceMetricSnapshot& persistence,
        const PerformanceMetricSnapshot& compose,
        const PerformanceMetricSnapshot& overlay)
    {
        double cursorUs = 0.0;

        drawCostSegment(
            painter, barRect, cursorUs,
            analyzer,
            QColor(205, 225, 235),
            QStringLiteral("A"));

        drawCostSegment(
            painter, barRect, cursorUs,
            persistence,
            QColor(185, 225, 205),
            QStringLiteral("P"));

        drawCostSegment(
            painter, barRect, cursorUs,
            compose,
            QColor(205, 225, 185),
            QStringLiteral("C"));

        drawCostSegment(
            painter, barRect, cursorUs,
            overlay,
            QColor(225, 205, 205),
            QStringLiteral("O"));

        const double remainderUs =
            std::max(
                static_cast<double>(total.latestUs) -
                    cursorUs,
                0.0);

        if (remainderUs > 0.0)
        {
            drawSegment(
                painter,
                barRect,
                cursorUs,
                remainderUs,
                QColor(130, 130, 130),
                QStringLiteral("?"));
        }
    }

    QString metricSummary(
        const QString& name,
        const PerformanceMetricSnapshot& metric)
    {
        return QStringLiteral(
            "%1\nlatest %2 ms   avg %3 ms   min %4 ms   max %5 ms")
            .arg(name)
            .arg(metric.latestMs(), 0, 'f', 2)
            .arg(metric.averageMs(), 0, 'f', 2)
            .arg(metric.minMs(), 0, 'f', 2)
            .arg(metric.maxMs(), 0, 'f', 2);
    }

    QString displayTimelineDetails(
        const QString& title,
        const DisplayPhaseTimelineSnapshot& timeline)
    {
        QStringList lines;
        lines << title;
        lines << QStringLiteral(
            "video critical path + opportunistic instrument work");

        lines << QStringLiteral(
            "generation %1   capture-relative timeline")
            .arg(timeline.generation);

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < DisplayPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];
            const char phase =
                static_cast<char>(event.phase);

            lines << QStringLiteral(
                "%1  %2   start %3 ms   duration %4 ms")
                .arg(QString::fromLatin1(displayPhaseText(phase)))
                .arg(QString::fromLatin1(displayPhaseName(phase)))
                .arg(
                    static_cast<double>(event.startUs) / 1000.0,
                    0, 'f', 2)
                .arg(
                    static_cast<double>(event.durationUs) / 1000.0,
                    0, 'f', 2);
        }

        if (timeline.count == 0)
        {
            lines << QStringLiteral("inactive / no published phases");
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString frequencyStateLine(
        const PerformanceSnapshot& snapshot)
    {
        switch (snapshot.frequencyCompensationState)
        {
        case FrequencyCompensationState::NoConsumer:
            return QStringLiteral(
                "F: No consumer; not running");

        case FrequencyCompensationState::Disabled:
            return QStringLiteral(
                "F: Disabled; not running");

        case FrequencyCompensationState::Completed:
            return QStringLiteral(
                "F complete @ %1 ms")
                .arg(
                    static_cast<double>(
                        snapshot.frequencyCompensationCompleteUs) / 1000.0,
                    0, 'f', 2);
        }

        return QStringLiteral("F: unknown state");
    }

    QString frequencyWorkerDetails(
        const PerformanceSnapshot& snapshot,
        std::size_t workerIndex)
    {
        QStringList lines;
        lines << frequencyStateLine(snapshot);

        if (workerIndex >=
            snapshot.frequencyWorkerPhases.size())
        {
            return lines.join(QLatin1Char('\n'));
        }

        const auto& timeline =
            snapshot.frequencyWorkerPhases[workerIndex];

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = timeline.events[i];

            lines << QStringLiteral(
                "F  Frequency compensation   start %1 ms   duration %2 ms")
                .arg(
                    static_cast<double>(event.startUs) / 1000.0,
                    0, 'f', 2)
                .arg(
                    static_cast<double>(event.durationUs) / 1000.0,
                    0, 'f', 2);
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString displayWorkerDetails(
        const PerformanceSnapshot& snapshot,
        std::size_t workerIndex)
    {
        QStringList lines;

        const auto& displayTimeline =
            workerIndex == 0u
            ? snapshot.displayWorker0Phases
            : snapshot.displayWorker1Phases;

        const auto& assistTimeline =
            workerIndex == 0u
            ? snapshot.displayWorker0Assist
            : snapshot.displayWorker1Assist;

        const auto& frequencyTimeline =
            snapshot.frequencyWorkerPhases[
                std::min<std::size_t>(
                    workerIndex,
                    snapshot.frequencyWorkerPhases.size() - 1u)];

        lines << (
            workerIndex == 0u
            ? QStringLiteral("Video worker 1")
            : QStringLiteral("Video worker 2"));

        lines << QStringLiteral(
            "generation %1   capture-relative timeline")
            .arg(
                displayTimeline.generation != 0u
                ? displayTimeline.generation
                : assistTimeline.generation);

        struct DetailEvent
        {
            char phase = '?';
            std::uint32_t startUs = 0;
            std::uint32_t durationUs = 0;
            bool waveformPhase = false;
            bool frequencyPhase = false;
            std::uint32_t chunkIndex = 0;
        };

        constexpr std::size_t kDetailCapacity =
            DisplayPhaseTimelineSnapshot::kCapacity +
            2u * WaveformAssistTimelineSnapshot::kCapacity;

        std::array<DetailEvent, kDetailCapacity> events{};
        std::size_t count = 0;

        for (std::uint32_t i = 0;
            i < displayTimeline.count &&
            i < DisplayPhaseTimelineSnapshot::kCapacity &&
            count < events.size();
            ++i)
        {
            const auto& event = displayTimeline.events[i];
            events[count++] =
                {
                    static_cast<char>(event.phase),
                    event.startUs,
                    event.durationUs,
                    false,
                    false,
                    0u
                };
        }

        for (std::uint32_t i = 0;
            i < assistTimeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity &&
            count < events.size();
            ++i)
        {
            const auto& event = assistTimeline.events[i];
            events[count++] =
                {
                    static_cast<char>(event.phase),
                    event.startUs,
                    event.durationUs,
                    true,
                    false,
                    event.jobIndex
                };
        }

        for (std::uint32_t i = 0;
            i < frequencyTimeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity &&
            count < events.size();
            ++i)
        {
            const auto& event = frequencyTimeline.events[i];
            events[count++] =
                {
                    static_cast<char>(event.phase),
                    event.startUs,
                    event.durationUs,
                    true,
                    true,
                    event.jobIndex
                };
        }

        std::stable_sort(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const DetailEvent& a, const DetailEvent& b)
            {
                if (a.startUs != b.startUs)
                {
                    return a.startUs < b.startUs;
                }
                return a.durationUs > b.durationUs;
            });

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& event = events[i];
            const char* text =
                event.waveformPhase
                ? waveformPhaseText(event.phase)
                : displayPhaseText(event.phase);
            const QString name =
                event.frequencyPhase
                ? QStringLiteral("Frequency compensation chunk %1")
                    .arg(event.chunkIndex)
                : event.waveformPhase
                    ? waveformAssistDescription(
                        event.phase,
                        event.chunkIndex)
                    : QString::fromLatin1(
                        displayPhaseName(event.phase));

            lines << QStringLiteral(
                "%1  %2   start %3 ms   duration %4 ms")
                .arg(QString::fromLatin1(text))
                .arg(name)
                .arg(
                    static_cast<double>(event.startUs) / 1000.0,
                    0, 'f', 2)
                .arg(
                    static_cast<double>(event.durationUs) / 1000.0,
                    0, 'f', 2);
        }

        if (count == 0u)
        {
            lines << QStringLiteral(
                "inactive / no published worker tasks");
        }

        lines << QStringLiteral("");
        lines << frequencyStateLine(snapshot);

        return lines.join(QLatin1Char('\n'));
    }

    QString workerTimelineDetails(
        const QString& title,
        const WorkerPhaseTimelineSnapshot& timeline,
        bool waveform)
    {
        QStringList lines;
        lines << title;
        lines << QStringLiteral(
            "ONE worker/thread - phases below execute serially");
        lines << QStringLiteral(
            "generation %1   capture-relative timeline")
            .arg(timeline.generation);

        std::array<DisplayPhaseEventSnapshot,
            WorkerPhaseTimelineSnapshot::kCapacity> events{};
        const std::size_t eventCount =
            std::min<std::size_t>(
                timeline.count,
                WorkerPhaseTimelineSnapshot::kCapacity);

        for (std::size_t i = 0; i < eventCount; ++i)
        {
            events[i] = timeline.events[i];
        }

        std::stable_sort(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(eventCount),
            [](const DisplayPhaseEventSnapshot& a,
                const DisplayPhaseEventSnapshot& b)
            {
                if (a.startUs != b.startUs)
                {
                    return a.startUs < b.startUs;
                }
                return a.durationUs > b.durationUs;
            });

        for (std::size_t i = 0; i < eventCount; ++i)
        {
            const auto& event = events[i];
            const char phase = static_cast<char>(event.phase);

            if (phase == '^')
            {
                continue;
            }

            const char* name =
                waveform
                    ? waveformWorkerPhaseName(phase)
                    : vectorscopeWorkerPhaseName(phase);

            lines << QStringLiteral(
                "%1  %2   start %3 ms   duration %4 ms")
                .arg(QString::fromLatin1(workerPhaseText(phase)))
                .arg(QString::fromLatin1(name))
                .arg(
                    static_cast<double>(event.startUs) / 1000.0,
                    0, 'f', 2)
                .arg(
                    static_cast<double>(event.durationUs) / 1000.0,
                    0, 'f', 2);
        }

        if (timeline.count == 0)
        {
            lines << QStringLiteral("inactive / no published worker phases");
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString vectorscopeWorkerDetails(
        const PerformanceSnapshot& snapshot)
    {
        QStringList lines;
        lines << QStringLiteral(
            "Vectorscope worker: authoritative wallclock chronology");
        lines << QStringLiteral(
            "start-end | worker | phase | duration | description");

        struct Event
        {
            char phase = '?';
            std::uint32_t startUs = 0;
            std::uint32_t durationUs = 0;
            bool assist = false;
            std::uint32_t chunkIndex = 0;
            std::uint32_t sequence = 0;
        };

        constexpr std::size_t kCapacity =
            WorkerPhaseTimelineSnapshot::kCapacity +
            WaveformAssistTimelineSnapshot::kCapacity;
        std::array<Event, kCapacity> events{};
        std::size_t count = 0;
        std::uint32_t sequence = 0;

        for (std::uint32_t i = 0;
            i < snapshot.vectorscopeWorkerPhases.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = snapshot.vectorscopeWorkerPhases.events[i];
            const char phase = static_cast<char>(event.phase);
            if (phase == '^')
            {
                continue;
            }
            events[count++] = {
                phase, event.startUs, event.durationUs,
                false, 0u, sequence++ };
        }

        const bool vectorscopeAssistIsCurrent =
            snapshot.waveformWorkerAssist.generation == 0u ||
            snapshot.vectorscopeWorkerAssist.generation ==
                snapshot.waveformWorkerAssist.generation;

        for (std::uint32_t i = 0;
            vectorscopeAssistIsCurrent &&
            i < snapshot.vectorscopeWorkerAssist.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = snapshot.vectorscopeWorkerAssist.events[i];
            events[count++] = {
                static_cast<char>(event.phase),
                event.startUs, event.durationUs,
                true, event.jobIndex, sequence++ };
        }

        std::stable_sort(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const Event& a, const Event& b)
            {
                if (a.startUs != b.startUs)
                {
                    return a.startUs < b.startUs;
                }
                return a.sequence < b.sequence;
            });

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& event = events[i];
            const double startMs =
                static_cast<double>(event.startUs) / 1000.0;
            const double endMs =
                static_cast<double>(event.startUs + event.durationUs) / 1000.0;
            const double durationMs =
                static_cast<double>(event.durationUs) / 1000.0;
            const QString description =
                event.assist
                ? waveformAssistDescription(event.phase, event.chunkIndex)
                : QString::fromLatin1(vectorscopeWorkerPhaseName(event.phase));

            lines << QStringLiteral(
                "%1-%2 | VS | %3 | %4 ms | %5")
                .arg(startMs, 0, 'f', 2)
                .arg(endMs, 0, 'f', 2)
                .arg(QString::fromLatin1(workerPhaseText(event.phase)))
                .arg(durationMs, 0, 'f', 2)
                .arg(description);
        }

        if (count == 0u)
        {
            lines << QStringLiteral(
                "inactive / no published worker phases");
        }

        return lines.join(QLatin1Char('\n'));
    }


    QString timingDiagnosticDetails(
        const PerformanceSnapshot& snapshot)
    {
        const auto formatEvent =
            [](const QString& name,
                const TimingDiagnosticEventSnapshot& event,
                const QString& valueName)
            {
                const QString interval =
                    event.intervalUs != 0u
                    ? QStringLiteral("%1 ms")
                        .arg(
                            static_cast<double>(event.intervalUs) / 1000.0,
                            0, 'f', 2)
                    : QStringLiteral("-");

                return QStringLiteral(
                    "%1: count %2   last interval %3   %4 %5")
                    .arg(name)
                    .arg(event.count)
                    .arg(interval)
                    .arg(valueName)
                    .arg(event.value);
            };

        QStringList lines;
        lines << QStringLiteral(
            "Timing diagnostics - rare events only; a ~1000 ms interval is the heartbeat smoking gun.");
        lines << formatEvent(
            QStringLiteral("RA  presenter re-anchor"),
            snapshot.presenterReanchor,
            QStringLiteral("late-us"));
        lines << formatEvent(
            QStringLiteral("PS  presenter generation skip"),
            snapshot.presenterGenerationSkip,
            QStringLiteral("frames"));
        lines << formatEvent(
            QStringLiteral("WS  waveform generation skip"),
            snapshot.waveformGenerationSkip,
            QStringLiteral("frames"));
        lines << formatEvent(
            QStringLiteral("VS  vectorscope generation skip"),
            snapshot.vectorscopeGenerationSkip,
            QStringLiteral("frames"));
        lines << QStringLiteral(
            "Trace sentinels: A001=RA, A002=PS, A003=WS, A004=VS.");

        return lines.join(QLatin1Char('\n'));
    }

    QString waveformWorkerDetails(
        const PerformanceSnapshot& snapshot)
    {
        QStringList lines;
        lines << QStringLiteral("Waveform worker: authoritative wallclock chronology");
        lines << QStringLiteral("start-end | worker | phase | duration | description");
        lines << QStringLiteral("generation %1   capture-relative timeline")
            .arg(snapshot.waveformWorkerPhases.generation);

        struct DetailEvent
        {
            char phase = '?';
            std::uint32_t startUs = 0;
            std::uint32_t durationUs = 0;
            const char* worker = "?";
            const char* description = "Unknown";
            bool assist = false;
            std::uint32_t chunkIndex = 0;
            std::uint32_t sequence = 0;
        };

        constexpr std::size_t kDetailCapacity =
            WorkerPhaseTimelineSnapshot::kCapacity +
            2u * WaveformPhaseTimelineSnapshot::kCapacity +
            4u * WaveformAssistTimelineSnapshot::kCapacity;

        std::array<DetailEvent, kDetailCapacity> events{};
        std::size_t count = 0;
        std::uint32_t sequence = 0;

        // S and V are wrapper envelopes. Their real Screen/Spout sub-phases are
        // added below, so do not mix either wrapper into the chronological stream.
        const auto& workerTimeline = snapshot.waveformWorkerPhases;
        for (std::uint32_t i = 0;
            i < workerTimeline.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = workerTimeline.events[i];
            const char phase = static_cast<char>(event.phase);
            if (phase == '^' || phase == 'S' || phase == 'V')
            {
                continue;
            }

            events[count++] = {
                phase,
                event.startUs,
                event.durationUs,
                "WF",
                waveformWorkerPhaseName(phase),
                false,
                0u,
                sequence++ };
        }

        // R and X in waveformScreenPhases are wallclock envelopes around
        // parallel work.  The actual jobs, their worker and chunk index come
        // from the three assist timelines below, so suppress the aggregates.
        const auto& phases = snapshot.waveformScreenPhases;
        for (std::uint32_t i = 0;
            i < phases.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = phases.events[i];
            const char phase = static_cast<char>(event.label);
            if (phase == 'R' || phase == 'X')
            {
                continue;
            }

            events[count++] = {
                phase,
                event.startUs,
                event.durationUs,
                "WF",
                waveformPhaseName(phase),
                false,
                0u,
                sequence++ };
        }

        // Spout waveform is serial in Delta 55, but its real renderer
        // sub-phases must appear in the same authoritative chronology as the
        // worker-row wapper.  V itself is only the outer envelope above.
        const auto& spoutPhases = snapshot.waveformVideoPhases;
        for (std::uint32_t i = 0;
            i < spoutPhases.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = spoutPhases.events[i];
            const char phase = static_cast<char>(event.label);
            if (phase == 'R' || phase == 'X')
            {
                continue;
            }
            events[count++] = {
                phase,
                event.startUs,
                event.durationUs,
                "WV",
                waveformPhaseName(phase),
                false,
                0u,
                sequence++ };
        }

        const std::uint64_t currentAssistGeneration =
            snapshot.waveformWorkerAssist.generation;

        const auto appendAssist =
            [&](const WaveformAssistTimelineSnapshot& assist,
                const char* workerName)
            {
                if (currentAssistGeneration != 0u &&
                    assist.generation != currentAssistGeneration)
                {
                    return;
                }

                for (std::uint32_t i = 0;
                    i < assist.count &&
                    i < WaveformAssistTimelineSnapshot::kCapacity &&
                    count < events.size(); ++i)
                {
                    const auto& event = assist.events[i];
                    const char phase = static_cast<char>(event.phase);
                    events[count++] = {
                        phase,
                        event.startUs,
                        event.durationUs,
                        workerName,
                        waveformPhaseName(phase),
                        true,
                        event.jobIndex,
                        sequence++ };
                }
            };

        appendAssist(snapshot.waveformWorkerAssist, "WF");
        appendAssist(snapshot.displayWorker0Assist, "V1");
        appendAssist(snapshot.displayWorker1Assist, "V2");
        appendAssist(snapshot.vectorscopeWorkerAssist, "VS");

        std::stable_sort(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const DetailEvent& a, const DetailEvent& b)
            {
                if (a.startUs != b.startUs)
                {
                    return a.startUs < b.startUs;
                }
                if (a.durationUs != b.durationUs)
                {
                    return a.durationUs > b.durationUs;
                }
                return a.sequence < b.sequence;
            });

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& event = events[i];
            const double startMs =
                static_cast<double>(event.startUs) / 1000.0;
            const double endMs =
                static_cast<double>(event.startUs + event.durationUs) / 1000.0;
            const double durationMs =
                static_cast<double>(event.durationUs) / 1000.0;
            const QString description =
                event.assist
                ? waveformAssistDescription(
                    event.phase,
                    event.chunkIndex)
                : QString::fromLatin1(event.description);

            lines << QStringLiteral("%1-%2 ms | %3 | %4 | %5 ms | %6")
                .arg(startMs, 0, 'f', 2)
                .arg(endMs, 0, 'f', 2)
                .arg(QString::fromLatin1(event.worker))
                .arg(QChar::fromLatin1(
                    event.phase == 'r' ? 'R' :
                    event.phase == 'x' ? 'X' : event.phase))
                .arg(durationMs, 0, 'f', 2)
                .arg(description);
        }

        if (count == 0u)
        {
            lines << QStringLiteral("inactive / no published waveform-worker events");
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString waveformScreenDetails(
        const PerformanceSnapshot& snapshot)
    {
        QStringList lines;
        lines << metricSummary(
            QStringLiteral("Screen waveform [timeline]"),
            snapshot.waveformScreen);
        lines << QStringLiteral(
            "start-end | worker | phase | duration | description");

        struct DetailEvent
        {
            char phase = '?';
            std::uint32_t startUs = 0;
            std::uint32_t durationUs = 0;
            const char* worker = "WF";
            bool assist = false;
            std::uint32_t chunkIndex = 0;
            std::uint32_t sequence = 0;
        };

        constexpr std::size_t kDetailCapacity =
            WaveformPhaseTimelineSnapshot::kCapacity +
            4u * WaveformAssistTimelineSnapshot::kCapacity;
        std::array<DetailEvent, kDetailCapacity> events{};
        std::size_t count = 0;
        std::uint32_t sequence = 0;

        const auto& phases = snapshot.waveformScreenPhases;
        for (std::uint32_t i = 0;
            i < phases.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity &&
            count < events.size(); ++i)
        {
            const auto& event = phases.events[i];
            const char phase = static_cast<char>(event.label);
            if (phase == 'F' || phase == 'R' || phase == 'X')
            {
                continue;
            }

            events[count++] = {
                phase,
                event.startUs,
                event.durationUs,
                "WF",
                false,
                0u,
                sequence++ };
        }

        const std::uint64_t detailAssistGeneration =
            snapshot.waveformWorkerAssist.generation;

        const auto appendAssist =
            [&](const WaveformAssistTimelineSnapshot& assist,
                const char* workerName)
            {
                if (detailAssistGeneration != 0u &&
                    assist.generation != detailAssistGeneration)
                {
                    return;
                }

                for (std::uint32_t i = 0;
                    i < assist.count &&
                    i < WaveformAssistTimelineSnapshot::kCapacity &&
                    count < events.size(); ++i)
                {
                    const auto& event = assist.events[i];
                    const char phase = static_cast<char>(event.phase);
                    if (phase != 'R' && phase != 'X')
                    {
                        continue;
                    }

                    events[count++] = {
                        phase,
                        event.startUs,
                        event.durationUs,
                        workerName,
                        true,
                        event.jobIndex,
                        sequence++ };
                }
            };

        appendAssist(snapshot.waveformWorkerAssist, "WF");
        appendAssist(snapshot.displayWorker0Assist, "V1");
        appendAssist(snapshot.displayWorker1Assist, "V2");
        appendAssist(snapshot.vectorscopeWorkerAssist, "VS");

        std::stable_sort(
            events.begin(),
            events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const DetailEvent& a, const DetailEvent& b)
            {
                if (a.startUs != b.startUs)
                {
                    return a.startUs < b.startUs;
                }
                if (a.durationUs != b.durationUs)
                {
                    return a.durationUs > b.durationUs;
                }
                return a.sequence < b.sequence;
            });

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& event = events[i];
            const double startMs =
                static_cast<double>(event.startUs) / 1000.0;
            const double endMs =
                static_cast<double>(event.startUs + event.durationUs) / 1000.0;
            const double durationMs =
                static_cast<double>(event.durationUs) / 1000.0;
            const QString description =
                event.assist
                ? waveformAssistDescription(
                    event.phase,
                    event.chunkIndex)
                : QString::fromLatin1(
                    waveformPhaseName(event.phase));

            lines << QStringLiteral("%1-%2 ms | %3 | %4 | %5 ms | %6")
                .arg(startMs, 0, 'f', 2)
                .arg(endMs, 0, 'f', 2)
                .arg(QString::fromLatin1(event.worker))
                .arg(QChar::fromLatin1(event.phase))
                .arg(durationMs, 0, 'f', 2)
                .arg(description);
        }

        if (count == 0u)
        {
            lines << QStringLiteral("inactive / no published Screen-waveform events");
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString waveformSpoutDetails(
        const PerformanceSnapshot& snapshot)
    {
        QStringList lines;
        lines << metricSummary(
            QStringLiteral("Spout waveform [timeline]"),
            snapshot.waveformVideo);

        // First show the authoritative outer V envelope.
        for (std::uint32_t i = 0;
            i < snapshot.waveformWorkerPhases.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = snapshot.waveformWorkerPhases.events[i];
            if (static_cast<char>(event.phase) != 'V')
            {
                continue;
            }

            const double startMs =
                static_cast<double>(event.startUs) / 1000.0;
            const double endMs =
                static_cast<double>(event.startUs + event.durationUs) / 1000.0;
            const double durationMs =
                static_cast<double>(event.durationUs) / 1000.0;
            lines << QStringLiteral(
                "%1-%2 ms | WF | V | %3 ms | Spout waveform render envelope")
                .arg(startMs, 0, 'f', 2)
                .arg(endMs, 0, 'f', 2)
                .arg(durationMs, 0, 'f', 2);
            break;
        }

        lines << QStringLiteral("");
        lines << QStringLiteral(
            "Spout renderer sub-phases (capture-relative chronology):");

        bool foundDetail = false;
        const auto& phases = snapshot.waveformVideoPhases;
        for (std::uint32_t i = 0;
            i < phases.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            const auto& event = phases.events[i];
            const char phase = static_cast<char>(event.label);
            if (phase == 'R' || phase == 'X')
            {
                continue;
            }
            const double startMs =
                static_cast<double>(event.startUs) / 1000.0;
            const double endMs =
                static_cast<double>(event.startUs + event.durationUs) / 1000.0;
            const double durationMs =
                static_cast<double>(event.durationUs) / 1000.0;
            lines << QStringLiteral(
                "%1-%2 ms | WV | %3 | %4 ms | %5")
                .arg(startMs, 0, 'f', 2)
                .arg(endMs, 0, 'f', 2)
                .arg(QChar::fromLatin1(phase))
                .arg(durationMs, 0, 'f', 2)
                .arg(QString::fromLatin1(waveformPhaseName(phase)));
            foundDetail = true;
        }

        const auto appendSpoutAssist =
            [&](const WaveformAssistTimelineSnapshot& assist,
                const char* workerName)
            {
                for (std::uint32_t i = 0;
                    i < assist.count &&
                    i < WaveformAssistTimelineSnapshot::kCapacity;
                    ++i)
                {
                    const auto& event = assist.events[i];
                    const char assistPhase = static_cast<char>(event.phase);
                    if (assistPhase != 'r' && assistPhase != 'x')
                    {
                        continue;
                    }
                    const double startMs = static_cast<double>(event.startUs) / 1000.0;
                    const double endMs = static_cast<double>(event.startUs + event.durationUs) / 1000.0;
                    const double durationMs = static_cast<double>(event.durationUs) / 1000.0;
                    const char visiblePhase = assistPhase == 'r' ? 'R' : 'X';
                    lines << QStringLiteral(
                        "%1-%2 ms | %3 | %4 | %5 ms | %6")
                        .arg(startMs, 0, 'f', 2)
                        .arg(endMs, 0, 'f', 2)
                        .arg(QString::fromLatin1(workerName))
                        .arg(QChar::fromLatin1(visiblePhase))
                        .arg(durationMs, 0, 'f', 2)
                        .arg(waveformAssistDescription(assistPhase, event.jobIndex));
                    foundDetail = true;
                }
            };

        appendSpoutAssist(snapshot.waveformWorkerAssist, "WF");
        appendSpoutAssist(snapshot.displayWorker0Assist, "V1");
        appendSpoutAssist(snapshot.displayWorker1Assist, "V2");
        appendSpoutAssist(snapshot.vectorscopeWorkerAssist, "VS");

        if (!foundDetail)
        {
            lines << QStringLiteral(
                "inactive / no published Spout renderer sub-phases");
        }

        return lines.join(QLatin1Char('\n'));
    }

    QString vectorscopeDetails(
        const QString& title,
        const PerformanceMetricSnapshot& total,
        const PerformanceMetricSnapshot& analyzer,
        const PerformanceMetricSnapshot& persistence,
        const PerformanceMetricSnapshot& compose,
        const PerformanceMetricSnapshot& overlay)
    {
        const double knownMs =
            analyzer.latestMs() +
            persistence.latestMs() +
            compose.latestMs() +
            overlay.latestMs();

        const double otherMs =
            std::max(
                total.latestMs() - knownMs,
                0.0);

        QStringList lines;
        lines << metricSummary(title, total);
        lines << QStringLiteral(
            "COST BREAKDOWN - not a capture-relative sub-phase timeline");
        lines << QStringLiteral("A analyzer/density: %1 ms")
            .arg(analyzer.latestMs(), 0, 'f', 2);
        lines << QStringLiteral("P persistence/glow: %1 ms")
            .arg(persistence.latestMs(), 0, 'f', 2);
        lines << QStringLiteral("C compose: %1 ms")
            .arg(compose.latestMs(), 0, 'f', 2);
        lines << QStringLiteral("O targets/overlay: %1 ms")
            .arg(overlay.latestMs(), 0, 'f', 2);
        lines << QStringLiteral("? unclassified: %1 ms")
            .arg(otherMs, 0, 'f', 2);

        return lines.join(QLatin1Char('\n'));
    }

    QString rowDetails(
        const PerformanceSnapshot& snapshot,
        int rowIndex)
    {
        if (rowIndex < 0 ||
            rowIndex >= static_cast<int>(kRows.size()))
        {
            return {};
        }

        switch (kRows[static_cast<std::size_t>(rowIndex)].kind)
        {
        case RowKind::FieldTiming:
            if (snapshot.field1Ready.latestUs == 0u &&
                snapshot.field2Ready.latestUs == 0u)
            {
                return QStringLiteral(
                    "Field timing\ninactive / no video consumer");
            }

            return QStringLiteral(
                "Field timing\n"
                "F1 present %1 ms (target 40 ms)\n"
                "F2 present %2 ms (target 60 ms)\n"
                "F1 -> F2 interval %3 ms\n"
                "deadline misses F1 %4   F2 %5")
                .arg(snapshot.field1Present.latestMs(), 0, 'f', 2)
                .arg(snapshot.field2Present.latestMs(), 0, 'f', 2)
                .arg(snapshot.presentInterval.latestMs(), 0, 'f', 2)
                .arg(snapshot.field1DeadlineMisses)
                .arg(snapshot.field2DeadlineMisses);

        case RowKind::TimingDiagnostics:
            return timingDiagnosticDetails(snapshot);

        case RowKind::Field1Ready:
            if (snapshot.field1Ready.latestUs == 0u &&
                snapshot.field2Ready.latestUs == 0u)
            {
                return QStringLiteral(
                    "Field 1 ready\ninactive / no video consumer");
            }

            return displayTimelineDetails(
                QStringLiteral("Field 1 ready - actual display pipeline phases"),
                snapshot.displayFieldPhases);

        case RowKind::Field2Ready:
            if (snapshot.field1Ready.latestUs == 0u &&
                snapshot.field2Ready.latestUs == 0u)
            {
                return QStringLiteral(
                    "Field 2 ready\ninactive / no video consumer");
            }

            return displayTimelineDetails(
                QStringLiteral("Field 2 ready - actual display pipeline phases"),
                snapshot.displayFieldPhases);

        case RowKind::DisplayWorker0:
            return displayWorkerDetails(
                snapshot,
                0u);

        case RowKind::DisplayWorker1:
            return displayWorkerDetails(
                snapshot,
                1u);

        case RowKind::ScreenVideo:
        {
            if (snapshot.field1Ready.latestUs == 0u &&
                snapshot.field2Ready.latestUs == 0u)
            {
                return QStringLiteral(
                    "Screen Frame Cost [wallclock]\n"
                    "inactive / no video consumer\n"
                    "wallclock cost 0.00 ms");
            }

            QStringList lines;
            lines << metricSummary(
                QStringLiteral(
                    "Screen Frame Cost [wallclock]"),
                snapshot.field2Ready);

            lines << QStringLiteral(
                "Definition: capture/timeline origin -> Field 2 ready.");
            lines << QStringLiteral(
                "This is the critical-path wallclock cost of completing the screen frame.");
            lines << QStringLiteral(
                "Video workers execute in parallel, so their CPU work is NOT summed here.");

            return lines.join(QLatin1Char('\n'));
        }

        case RowKind::WaveformWorker:
            return waveformWorkerDetails(snapshot);

        case RowKind::WaveformScreen:
            return waveformScreenDetails(snapshot);

        case RowKind::WaveformSpout:
            return waveformSpoutDetails(snapshot);

        case RowKind::VectorscopeWorker:
            return vectorscopeWorkerDetails(snapshot);

        case RowKind::VectorscopeScreen:
            return vectorscopeDetails(
                QStringLiteral("Screen vectorscope"),
                snapshot.vectorscopeScreen,
                snapshot.vectorscopeScreenAnalyzer,
                snapshot.vectorscopeScreenGlowPersistence,
                snapshot.vectorscopeScreenCompose,
                snapshot.vectorscopeScreenOverlay);

        case RowKind::VectorscopeSpout:
            return vectorscopeDetails(
                QStringLiteral("Spout vectorscope"),
                snapshot.vectorscopeVideo,
                snapshot.vectorscopeVideoAnalyzer,
                snapshot.vectorscopeVideoGlowPersistence,
                snapshot.vectorscopeVideoCompose,
                snapshot.vectorscopeVideoOverlay);

        case RowKind::DisplayCompose:
        {
            QStringList lines;
            lines << metricSummary(
                QStringLiteral(
                    "Display compose CPU sum [work]"),
                snapshot.displayCompose);

            lines << QStringLiteral(
                "Definition: sum of composeUs over both fields and all display workers.");
            lines << QStringLiteral(
                "This is accumulated CPU work, NOT wallclock and NOT a worker completion time.");

            return lines.join(QLatin1Char('\n'));
        }
        }

        return {};
    }

    double displayTimelineEndUs(
        const DisplayPhaseTimelineSnapshot& timeline)
    {
        double endUs = 0.0;

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < DisplayPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            endUs = std::max(
                endUs,
                static_cast<double>(timeline.events[i].startUs) +
                static_cast<double>(timeline.events[i].durationUs));
        }

        return endUs;
    }

    double assistTimelineEndUs(
        const WaveformAssistTimelineSnapshot& timeline)
    {
        double endUs = 0.0;

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformAssistTimelineSnapshot::kCapacity;
            ++i)
        {
            endUs = std::max(
                endUs,
                static_cast<double>(timeline.events[i].startUs) +
                static_cast<double>(timeline.events[i].durationUs));
        }

        return endUs;
    }

    double workerTimelineEndUs(
        const WorkerPhaseTimelineSnapshot& timeline)
    {
        double endUs = 0.0;

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WorkerPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            endUs = std::max(
                endUs,
                static_cast<double>(timeline.events[i].startUs) +
                static_cast<double>(timeline.events[i].durationUs));
        }

        return endUs;
    }

    double waveformTimelineEndUs(
        const WaveformPhaseTimelineSnapshot& timeline)
    {
        double endUs = 0.0;

        for (std::uint32_t i = 0;
            i < timeline.count &&
            i < WaveformPhaseTimelineSnapshot::kCapacity;
            ++i)
        {
            endUs = std::max(
                endUs,
                static_cast<double>(timeline.events[i].startUs) +
                static_cast<double>(timeline.events[i].durationUs));
        }

        return endUs;
    }

    double maximumWorkerEndUs(
        const PerformanceSnapshot& snapshot)
    {
        return std::max(
        {
            displayTimelineEndUs(snapshot.displayWorker0Phases),
            assistTimelineEndUs(snapshot.displayWorker0Assist),
            displayTimelineEndUs(snapshot.displayWorker1Phases),
            assistTimelineEndUs(snapshot.displayWorker1Assist),
            workerTimelineEndUs(snapshot.waveformWorkerPhases),
            waveformTimelineEndUs(snapshot.waveformScreenPhases),
            assistTimelineEndUs(snapshot.waveformWorkerAssist),
            workerTimelineEndUs(snapshot.vectorscopeWorkerPhases),
            (snapshot.waveformWorkerAssist.generation == 0u ||
             snapshot.vectorscopeWorkerAssist.generation ==
                 snapshot.waveformWorkerAssist.generation)
                ? assistTimelineEndUs(snapshot.vectorscopeWorkerAssist)
                : 0.0
        });
    }

    int rowAtPosition(
        const QPoint& position)
    {
        const int firstRowY =
            kMargin +
            kToolbarHeight +
            kScaleHeight +
            kTimelineScrollHeight +
            4;

        const int relativeY =
            position.y() - firstRowY;

        if (relativeY < 0)
        {
            return -1;
        }

        const int rowHeight =
            kBarHeight + kRowSpacing;

        const int index =
            relativeY / rowHeight;

        if (index < 0 ||
            index >= static_cast<int>(kRows.size()) ||
            (relativeY % rowHeight) >= kBarHeight)
        {
            return -1;
        }

        return index;
    }
}

PerformanceWidget::PerformanceWidget(
    QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(
        1050,
        690);

    setMouseTracking(true);

    pauseButton_ =
        new QPushButton(
            QStringLiteral("Pause"),
            this);

    pauseButton_->setCheckable(true);

    connect(
        pauseButton_,
        &QPushButton::toggled,
        this,
        [this](bool paused)
        {
            paused_ = paused;

            pauseButton_->setText(
                paused_
                    ? QStringLiteral("Resume")
                    : QStringLiteral("Pause"));

            update();
        });

    timelineZoomSlider_ =
        new QSlider(
            Qt::Horizontal,
            this);

    timelineZoomSlider_->setRange(1, 16);
    timelineZoomSlider_->setValue(1);
    timelineZoomSlider_->setSingleStep(1);
    timelineZoomSlider_->setPageStep(1);

    connect(
        timelineZoomSlider_,
        &QSlider::valueChanged,
        this,
        [this](int zoom)
        {
            const double centerUs =
                timelineStartUs_ +
                timelineSpanUs_ * 0.5;

            timelineSpanUs_ =
                std::max(
                    kMaximumUs /
                        static_cast<double>(
                            std::max(zoom, 1)),
                    kMinimumTimelineSpanUs);

            timelineStartUs_ =
                std::clamp(
                    centerUs -
                        timelineSpanUs_ * 0.5,
                    0.0,
                    std::max(
                        kMaximumUs -
                            timelineSpanUs_,
                        0.0));

            updateTimelineControls();
            update();
        });

    timelineScrollBar_ =
        new QScrollBar(
            Qt::Horizontal,
            this);

    connect(
        timelineScrollBar_,
        &QScrollBar::valueChanged,
        this,
        [this](int value)
        {
            timelineStartUs_ =
                static_cast<double>(value);
            update();
        });

    detailScrollBar_ =
        new QScrollBar(
            Qt::Vertical,
            this);

    detailScrollBar_->setRange(0, 0);
    detailScrollBar_->setSingleStep(24);
    detailScrollBar_->setPageStep(120);

    connect(
        detailScrollBar_,
        &QScrollBar::valueChanged,
        this,
        [this](int value)
        {
            detailScrollOffset_ = value;
            update();
        });

    updateTimelineControls();
    layoutTimelineControls();
}

void PerformanceWidget::setPerformanceSnapshot(
    const PerformanceSnapshot& snapshot)
{
    if (paused_)
    {
        return;
    }

    snapshot_ = snapshot;

    if (autoPauseArmed_ &&
        maximumWorkerEndUs(snapshot_) >
            autoPauseThresholdUs_)
    {
        autoPauseArmed_ = false;
        paused_ = true;

        if (pauseButton_ != nullptr)
        {
            pauseButton_->setChecked(true);
            pauseButton_->setText(
                QStringLiteral("Resume"));
        }
    }

    update();
}

void PerformanceWidget::updateTimelineControls()
{
    if (timelineScrollBar_ == nullptr)
    {
        return;
    }

    const int maximumStartUs =
        static_cast<int>(
            std::max(
                kMaximumUs -
                    timelineSpanUs_,
                0.0));

    timelineScrollBar_->setRange(
        0,
        maximumStartUs);

    timelineScrollBar_->setPageStep(
        std::max(
            static_cast<int>(timelineSpanUs_),
            1));

    timelineScrollBar_->setSingleStep(
        std::max(
            static_cast<int>(timelineSpanUs_ / 20.0),
            100));

    timelineScrollBar_->setValue(
        std::clamp(
            static_cast<int>(
                std::lround(timelineStartUs_)),
            0,
            maximumStartUs));
}

void PerformanceWidget::layoutTimelineControls()
{
    if (pauseButton_ == nullptr ||
        timelineZoomSlider_ == nullptr ||
        timelineScrollBar_ == nullptr)
    {
        return;
    }

    const int top =
        kMargin;

    pauseButton_->setGeometry(
        kMargin,
        top,
        82,
        24);

    timelineZoomSlider_->setGeometry(
        kMargin + 160,
        top + 2,
        180,
        20);

    timelineScrollBar_->setGeometry(
        kMargin + 350,
        top + 3,
        std::max(
            width() -
                (kMargin + 350) -
                kMargin,
            120),
        kTimelineScrollHeight);
}

void PerformanceWidget::paintEvent(
    QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    gTimelineStartUs = timelineStartUs_;
    gTimelineSpanUs = timelineSpanUs_;

    const QFontMetrics fontMetrics(
        painter.font());

    int labelWidth = 0;
    for (const Row& row : kRows)
    {
        labelWidth =
            std::max(
                labelWidth,
                fontMetrics.horizontalAdvance(
                    QString::fromLatin1(row.label)));
    }

    labelWidth += 18;

    const int barLeft =
        kMargin + labelWidth;

    const int barWidth =
        std::max(
            width() - barLeft - kMargin,
            1);

    painter.setPen(Qt::lightGray);

    painter.drawText(
        QRect(
            kMargin + 92,
            kMargin,
            64,
            24),
        Qt::AlignLeft | Qt::AlignVCenter,
        paused_
            ? QStringLiteral("PAUSED")
            : QStringLiteral("LIVE"));

    painter.drawText(
        QRect(
            kMargin + 350 - 92,
            kMargin,
            88,
            24),
        Qt::AlignRight | Qt::AlignVCenter,
        QStringLiteral("Zoom x%1")
            .arg(
                timelineZoomSlider_ != nullptr
                    ? timelineZoomSlider_->value()
                    : 1));

    if (autoPauseArmed_)
    {
        painter.setPen(
            QColor(238, 210, 165));

        painter.drawText(
            QRect(
                kMargin + 350,
                kMargin,
                260,
                24),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral(
                "AUTO-PAUSE > %1 ms")
                .arg(
                    autoPauseThresholdUs_ / 1000.0,
                    0, 'f', 2));
    }

    const int scaleTop =
        kMargin +
        kToolbarHeight;

    const auto drawScale =
        [&](double us,
            const QString& text,
            Qt::Alignment alignment)
        {
            if (!isUsVisible(us))
            {
                return;
            }

            const int x =
                xForUs(
                    QRect(
                        barLeft,
                        scaleTop,
                        barWidth,
                        kBarHeight),
                    us);

            QRect scaleRect(
                x - 42,
                scaleTop,
                84,
                kBarHeight);

            if (alignment & Qt::AlignLeft)
            {
                scaleRect.moveLeft(x);
            }
            else if (alignment & Qt::AlignRight)
            {
                scaleRect.moveRight(x);
            }

            painter.drawText(
                scaleRect,
                alignment | Qt::AlignVCenter,
                text);
        };

    const double viewEndUs =
        timelineStartUs_ +
        timelineSpanUs_;

    // Adaptive time ruler. At high zoom Grobbeoog gets a real millisecond
    // ruler instead of having to infer time from sparse 5 ms labels.
    double majorTickUs = 20000.0;
    double minorTickUs = 5000.0;

    if (timelineSpanUs_ <= 12000.0)
    {
        majorTickUs = 1000.0;
        minorTickUs = 1000.0;
    }
    else if (timelineSpanUs_ <= 24000.0)
    {
        majorTickUs = 2000.0;
        minorTickUs = 1000.0;
    }
    else if (timelineSpanUs_ <= 45000.0)
    {
        majorTickUs = 5000.0;
        minorTickUs = 1000.0;
    }
    else
    {
        majorTickUs = 10000.0;
        minorTickUs = 5000.0;
    }

    const QRect rulerBarRect(
        barLeft,
        scaleTop,
        barWidth,
        kBarHeight);

    // Minor ticks: unobtrusive 1 ms marks when zoomed in.
    painter.setPen(QColor(105, 105, 105));
    const double firstMinorUs =
        std::ceil(timelineStartUs_ / minorTickUs) * minorTickUs;
    for (double us = firstMinorUs;
        us <= viewEndUs + 0.5;
        us += minorTickUs)
    {
        if (!isUsVisible(us))
        {
            continue;
        }

        const int x = xForUs(rulerBarRect, us);
        painter.drawLine(
            x,
            scaleTop + kBarHeight - 5,
            x,
            scaleTop + kBarHeight - 1);
    }

    painter.setPen(Qt::white);
    const double firstMajorUs =
        std::ceil(timelineStartUs_ / majorTickUs) * majorTickUs;
    bool firstVisibleTick = true;

    for (double us = firstMajorUs;
        us <= viewEndUs + 0.5;
        us += majorTickUs)
    {
        if (!isUsVisible(us))
        {
            continue;
        }

        const bool atRightEdge =
            std::abs(us - viewEndUs) < majorTickUs * 0.25;

        drawScale(
            us,
            QStringLiteral("%1 ms")
                .arg(us / 1000.0, 0, 'g', 5),
            firstVisibleTick
                ? Qt::AlignLeft
                : (atRightEdge
                    ? Qt::AlignRight
                    : Qt::AlignHCenter));

        firstVisibleTick = false;
    }

    int y =
        kMargin +
        kToolbarHeight +
        kScaleHeight +
        kTimelineScrollHeight +
        4;

    for (const Row& row : kRows)
    {
        painter.setPen(Qt::white);

        painter.drawText(
            QRect(
                kMargin,
                y,
                labelWidth,
                kBarHeight),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromLatin1(row.label));

        const QRect barRect(
            barLeft,
            y,
            barWidth,
            kBarHeight);

        painter.fillRect(
            barRect,
            QColor(24, 24, 24));

        switch (row.kind)
        {
        case RowKind::FieldTiming:
            if (snapshot_.field1Ready.latestUs == 0u &&
                snapshot_.field2Ready.latestUs == 0u)
            {
                painter.fillRect(
                    barRect,
                    QColor(55, 55, 55));

                painter.setPen(
                    QColor(170, 170, 170));

                painter.drawText(
                    barRect,
                    Qt::AlignCenter,
                    QStringLiteral(
                        "inactive / no video consumer"));

                break;
            }

            fillFieldTimingBackground(
                painter,
                barRect);

            drawFieldStatusMarker(
                painter,
                barRect,
                static_cast<double>(
                    snapshot_.field1Present.latestUs),
                kField1DeadlineUs);

            drawFieldStatusMarker(
                painter,
                barRect,
                static_cast<double>(
                    snapshot_.field2Present.latestUs),
                kField2DeadlineUs);
            break;

        case RowKind::TimingDiagnostics:
        {
            drawDeadlineGuides(painter, barRect);

            const struct
            {
                const char* label;
                const TimingDiagnosticEventSnapshot* event;
                QColor color;
            } diagnostics[] =
            {
                { "RA", &snapshot_.presenterReanchor, QColor(235, 150, 150) },
                { "PS", &snapshot_.presenterGenerationSkip, QColor(235, 205, 150) },
                { "WS", &snapshot_.waveformGenerationSkip, QColor(180, 215, 235) },
                { "VS", &snapshot_.vectorscopeGenerationSkip, QColor(205, 185, 235) }
            };

            int left = barRect.left() + 4;

            for (const auto& diagnostic : diagnostics)
            {
                if (diagnostic.event->count == 0u)
                {
                    continue;
                }

                const QString text =
                    QStringLiteral("%1:%2")
                        .arg(QString::fromLatin1(diagnostic.label))
                        .arg(diagnostic.event->count);

                const int markerWidth =
                    std::max(
                        painter.fontMetrics().horizontalAdvance(text) + 10,
                        42);

                const QRect marker(
                    left,
                    barRect.top() + 2,
                    markerWidth,
                    barRect.height() - 4);

                painter.fillRect(marker, diagnostic.color);
                painter.setPen(Qt::black);
                painter.drawText(marker, Qt::AlignCenter, text);

                left += markerWidth + 4;
            }

            break;
        }

        case RowKind::Field1Ready:
            if (snapshot_.field1Ready.latestUs == 0u &&
                snapshot_.field2Ready.latestUs == 0u)
            {
                painter.fillRect(
                    barRect,
                    QColor(55, 55, 55));

                painter.setPen(
                    QColor(170, 170, 170));

                painter.drawText(
                    barRect,
                    Qt::AlignCenter,
                    QStringLiteral("inactive"));

                break;
            }

            fillTimingBackground(
                painter,
                barRect,
                35000.0,
                40000.0);

            drawDisplayTimeline(
                painter,
                barRect,
                snapshot_.displayFieldPhases,
                false);
            break;

        case RowKind::Field2Ready:
            if (snapshot_.field1Ready.latestUs == 0u &&
                snapshot_.field2Ready.latestUs == 0u)
            {
                painter.fillRect(
                    barRect,
                    QColor(55, 55, 55));

                painter.setPen(
                    QColor(170, 170, 170));

                painter.drawText(
                    barRect,
                    Qt::AlignCenter,
                    QStringLiteral("inactive"));

                break;
            }

            fillTimingBackground(
                painter,
                barRect,
                55000.0,
                60000.0);

            drawDisplayTimeline(
                painter,
                barRect,
                snapshot_.displayFieldPhases,
                true);
            break;

        case RowKind::DisplayWorker0:
            drawDeadlineGuides(painter, barRect);
            drawDisplayTimeline(
                painter,
                barRect,
                snapshot_.displayWorker0Phases,
                true);
            drawAssistTimeline(
                painter,
                barRect,
                snapshot_.frequencyWorkerPhases[0]);
            drawAssistTimeline(
                painter,
                barRect,
                snapshot_.displayWorker0Assist);
            drawPriorityStateTimeline(
                painter,
                barRect,
                snapshot_.priorityVideo1);
            drawFrequencyPriorityTimeline(
                painter,
                barRect,
                snapshot_.frequencyWorkerPhases[0]);
            break;

        case RowKind::DisplayWorker1:
            drawDeadlineGuides(painter, barRect);
            drawDisplayTimeline(
                painter,
                barRect,
                snapshot_.displayWorker1Phases,
                true);
            drawAssistTimeline(
                painter,
                barRect,
                snapshot_.frequencyWorkerPhases[1]);
            drawAssistTimeline(
                painter,
                barRect,
                snapshot_.displayWorker1Assist);
            drawPriorityStateTimeline(
                painter,
                barRect,
                snapshot_.priorityVideo2);
            drawFrequencyPriorityTimeline(
                painter,
                barRect,
                snapshot_.frequencyWorkerPhases[1]);
            break;

        case RowKind::ScreenVideo:
            if (snapshot_.field1Ready.latestUs == 0u &&
                snapshot_.field2Ready.latestUs == 0u)
            {
                painter.fillRect(
                    barRect,
                    QColor(55, 55, 55));

                painter.setPen(
                    QColor(170, 170, 170));

                painter.drawText(
                    barRect,
                    Qt::AlignCenter,
                    QStringLiteral("inactive / 0.00 ms"));

                break;
            }

            drawDeadlineGuides(painter, barRect);
            drawSegment(
                painter,
                barRect,
                0.0,
                static_cast<double>(
                    snapshot_.field2Ready.latestUs),
                QColor(220, 220, 220),
                QStringLiteral("F2 ready"));
            break;

        case RowKind::WaveformWorker:
            drawDeadlineGuides(painter, barRect);

            // Top-level Screen / Spout / measurement / publish envelope.
            drawWorkerTimeline(
                painter,
                barRect,
                snapshot_.waveformWorkerPhases,
                true);

            // Exact Screen-render sub-phases belong on this worker chronology.
            drawWaveformWorkerScreenPhases(
                painter,
                barRect,
                snapshot_.waveformScreenPhases);

            // Delta 53: make the worker-row Spout section compatible with the
            // detailed child row. V stays as a dashed parent envelope while
            // its exact renderer phases fill the real chronology.
            drawWaveformWorkerSpoutPhases(
                painter,
                barRect,
                snapshot_.waveformVideoPhases);

            drawAssistTimeline(
                painter,
                barRect,
                snapshot_.waveformWorkerAssist);
            drawPriorityStateTimeline(
                painter,
                barRect,
                snapshot_.priorityWaveform);
            break;

        case RowKind::WaveformScreen:
            // Exact renderer chronology.  Do not synthesize an additive
            // remainder here: every published phase, including X, is drawn at
            // its real capture-relative position.
            drawWaveformScreenTimeline(
                painter,
                barRect,
                snapshot_);
            break;

        case RowKind::WaveformSpout:
            drawDeadlineGuides(painter, barRect);
            drawWaveformSpoutTimeline(
                painter,
                barRect,
                snapshot_);
            break;

        case RowKind::VectorscopeWorker:
            drawDeadlineGuides(painter, barRect);
            drawWorkerTimeline(
                painter,
                barRect,
                snapshot_.vectorscopeWorkerPhases);
            if (snapshot_.waveformWorkerAssist.generation == 0u ||
                snapshot_.vectorscopeWorkerAssist.generation ==
                    snapshot_.waveformWorkerAssist.generation)
            {
                drawAssistTimeline(
                    painter,
                    barRect,
                    snapshot_.vectorscopeWorkerAssist);
            }
            drawPriorityStateTimeline(
                painter,
                barRect,
                snapshot_.priorityVectorscope);
            break;

        case RowKind::VectorscopeScreen:
            drawDeadlineGuides(painter, barRect);
            drawVectorscopeCostBreakdown(
                painter,
                barRect,
                snapshot_.vectorscopeScreen,
                snapshot_.vectorscopeScreenAnalyzer,
                snapshot_.vectorscopeScreenGlowPersistence,
                snapshot_.vectorscopeScreenCompose,
                snapshot_.vectorscopeScreenOverlay);
            break;

        case RowKind::VectorscopeSpout:
            drawDeadlineGuides(painter, barRect);
            drawVectorscopeCostBreakdown(
                painter,
                barRect,
                snapshot_.vectorscopeVideo,
                snapshot_.vectorscopeVideoAnalyzer,
                snapshot_.vectorscopeVideoGlowPersistence,
                snapshot_.vectorscopeVideoCompose,
                snapshot_.vectorscopeVideoOverlay);
            break;

        case RowKind::DisplayCompose:
            // CPU-work sum: deliberately starts at zero.  This is not a
            // capture-relative chronology; the row name says so explicitly.
            drawSegment(
                painter,
                barRect,
                0.0,
                static_cast<double>(
                    snapshot_.displayCompose.latestUs),
                QColor(220, 220, 220),
                QStringLiteral("CPU"));
            break;
        }

        painter.setPen(Qt::gray);
        painter.drawRect(barRect);

        y +=
            kBarHeight +
            kRowSpacing;
    }

    if (autoPauseArmed_ &&
        autoPauseThresholdUs_ >= timelineStartUs_ &&
        autoPauseThresholdUs_ <=
            timelineStartUs_ + timelineSpanUs_)
    {
        const int triggerX =
            xForUs(
                QRect(
                    barLeft,
                    0,
                    barWidth,
                    kBarHeight),
                autoPauseThresholdUs_);

        QPen triggerPen(
            QColor(238, 210, 165));
        triggerPen.setWidth(2);
        triggerPen.setStyle(Qt::DashLine);
        painter.setPen(triggerPen);

        const int rowsTop =
            kMargin +
            kToolbarHeight +
            kScaleHeight +
            kTimelineScrollHeight +
            4;

        painter.drawLine(
            triggerX,
            rowsTop,
            triggerX,
            y - kRowSpacing);
    }

    painter.setPen(Qt::lightGray);

    painter.drawText(
        QRect(
            kMargin,
            y,
            width() - kMargin * 2,
            kBarHeight),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral(
            "Deadline misses: F1 %1   F2 %2")
            .arg(snapshot_.field1DeadlineMisses)
            .arg(snapshot_.field2DeadlineMisses));

    y += kBarHeight;

    painter.drawText(
        QRect(
            kMargin,
            y,
            width() - kMargin * 2,
            kBarHeight),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral(
            "Spout transport: queue %1 ms   send %2 ms   cadence %3 ms")
            .arg(snapshot_.spoutQueueDelay.latestMs(), 0, 'f', 2)
            .arg(snapshot_.spoutSend.latestMs(), 0, 'f', 2)
            .arg(snapshot_.spoutInterval.latestMs(), 0, 'f', 2));

    y +=
        kBarHeight +
        8;

    const QRect detailRect(
        kMargin,
        y,
        std::max(width() - kMargin * 2, 1),
        std::max(height() - y - kMargin, 120));

    if (detailScrollBar_ != nullptr)
    {
        detailScrollBar_->setGeometry(
            detailRect.right() - 14,
            detailRect.top() + 1,
            14,
            std::max(detailRect.height() - 2, 1));
    }

    painter.fillRect(
        detailRect,
        QColor(30, 30, 30));

    painter.setPen(
        QColor(95, 95, 95));

    painter.drawRect(
        detailRect.adjusted(0, 0, -1, -1));

    painter.setPen(
        QColor(220, 220, 220));

    QString detailText =
        QStringLiteral(
            "Left-click any bar to PIN that exact snapshot.\n"
            "Right-click a timing position to AUTO-PAUSE when any worker crosses it.\n"
            "Mouse-over uses the same reporting function as the pinned view.\n"
            "Pause freezes incoming snapshots; Zoom + scrollbar inspect the 0..80 ms timeline.");

    if (hasPinnedSnapshot_ &&
        pinnedRowIndex_ >= 0)
    {
        detailText =
            QStringLiteral("PINNED SNAPSHOT - click another bar to replace\n\n") +
            rowDetails(
                pinnedSnapshot_,
                pinnedRowIndex_);
    }

    const QRect detailTextRect =
        detailRect.adjusted(
            10,
            8,
            detailScrollBar_ != nullptr ? -28 : -10,
            -8);

    const QRect measuredTextRect =
        painter.boundingRect(
            QRect(0, 0, detailTextRect.width(), 100000),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            detailText);

    const int maximumDetailOffset =
        std::max(
            measuredTextRect.height() - detailTextRect.height(),
            0);

    if (detailScrollBar_ != nullptr)
    {
        detailScrollBar_->setRange(0, maximumDetailOffset);
        detailScrollBar_->setPageStep(
            std::max(detailTextRect.height(), 1));
        detailScrollBar_->setVisible(maximumDetailOffset > 0);

        if (detailScrollOffset_ > maximumDetailOffset)
        {
            detailScrollOffset_ = maximumDetailOffset;
            detailScrollBar_->setValue(maximumDetailOffset);
        }
    }

    painter.save();
    painter.setClipRect(detailTextRect);

    const QRect scrolledDetailTextRect(
        detailTextRect.left(),
        detailTextRect.top() - detailScrollOffset_,
        detailTextRect.width(),
        std::max(measuredTextRect.height(), detailTextRect.height()));

    painter.drawText(
        scrolledDetailTextRect,
        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
        detailText);
    painter.restore();
}

void PerformanceWidget::mouseMoveEvent(
    QMouseEvent* event)
{
    const int rowIndex =
        rowAtPosition(
            event->position().toPoint());

    if (rowIndex < 0)
    {
        QToolTip::hideText();
        return;
    }

    const QString detail =
        rowDetails(
            snapshot_,
            rowIndex);

    if (detail.isEmpty())
    {
        QToolTip::hideText();
        return;
    }

    QToolTip::showText(
        event->globalPosition().toPoint(),
        detail,
        this);
}

void PerformanceWidget::mousePressEvent(
    QMouseEvent* event)
{
    const QPoint position =
        event->position().toPoint();

    if (event->button() == Qt::RightButton)
    {
        const int rowIndex =
            rowAtPosition(position);

        if (rowIndex < 0)
        {
            QWidget::mousePressEvent(event);
            return;
        }

        const QFontMetrics fontMetrics(font());

        int labelWidth = 0;

        for (const Row& row : kRows)
        {
            labelWidth =
                std::max(
                    labelWidth,
                    fontMetrics.horizontalAdvance(
                        QString::fromLatin1(
                            row.label)));
        }

        labelWidth += 18;

        const int barLeft =
            kMargin + labelWidth;

        const int barWidth =
            std::max(
                width() -
                    barLeft -
                    kMargin,
                1);

        if (position.x() < barLeft ||
            position.x() > barLeft + barWidth)
        {
            QWidget::mousePressEvent(event);
            return;
        }

        const double fraction =
            std::clamp(
                static_cast<double>(
                    position.x() - barLeft) /
                static_cast<double>(
                    barWidth),
                0.0,
                1.0);

        autoPauseThresholdUs_ =
            timelineStartUs_ +
            fraction *
                timelineSpanUs_;

        autoPauseArmed_ = true;

        if (paused_)
        {
            paused_ = false;

            if (pauseButton_ != nullptr)
            {
                pauseButton_->setChecked(false);
                pauseButton_->setText(
                    QStringLiteral("Pause"));
            }
        }

        update();
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    const int rowIndex =
        rowAtPosition(position);

    if (rowIndex < 0)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    pinnedSnapshot_ = snapshot_;
    pinnedRowIndex_ = rowIndex;
    hasPinnedSnapshot_ = true;

    update();
    event->accept();
}

void PerformanceWidget::resizeEvent(
    QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutTimelineControls();
}

void PerformanceWidget::closeEvent(
    QCloseEvent* event)
{
    emit visibilityChanged(false);
    QWidget::closeEvent(event);
}

QSize PerformanceWidget::sizeHint() const
{
    const int rowsHeight =
        static_cast<int>(kRows.size()) *
            (kBarHeight + kRowSpacing);

    return QSize(
        1180,
        kMargin +
            kToolbarHeight +
            kScaleHeight +
            kTimelineScrollHeight +
            4 +
            rowsHeight +
            2 * kBarHeight +
            300);
}
