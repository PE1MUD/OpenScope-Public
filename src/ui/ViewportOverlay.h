#pragma once

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace ViewportOverlay
{
    inline QColor panelColor()
    {
        return QColor(68, 74, 78, 235);
    }

    inline QColor ownerPanelColor()
    {
        // Deliberately darker than the SNR-style cards so the three
        // information groups read as one instrument panel.
        return QColor(35, 39, 42, 235);
    }

    inline QColor textColor()
    {
        return QColor(248, 250, 250);
    }

    inline double panelBorderWidth(bool palOutput = false)
    {
        return palOutput ? 2.2 : 1.0;
    }

    inline double ownerPanelBorderWidth(bool palOutput = false)
    {
        return palOutput ? 2.4 : 1.0;
    }

    inline QFont fontForHeight(
        double height,
        bool bold = false,
        bool palOutput = false)
    {
        QFont font = QApplication::font();
        font.setBold(bold);

        // PAL/Spout uses the same relative label height as the waveform
        // graticule: 4.5% of the active presentation height. This keeps the
        // two instruments visually consistent and avoids wasting PAL canvas
        // on oversized metadata text. Screen typography remains unchanged.
        const int pixelSize =
            palOutput
            ? std::max(
                1,
                static_cast<int>(
                    std::lround(height * 0.045)))
            : std::clamp(
                static_cast<int>(
                    std::lround(height * 0.030)),
                9,
                26);

        font.setPixelSize(pixelSize);

        // PAL has very little horizontal room once the scope and underscan
        // are honoured. A mild condensed stretch preserves a large, CRT-
        // readable character height without turning the information panel
        // into most of the picture width. Screen typography is unchanged.
        if (palOutput)
        {
            font.setStretch(58);
        }

        return font;
    }

    inline void drawNoVideo(
        QPainter& painter,
        const QRectF& bounds,
        bool palOutput = false)
    {
        QFont font;

        if (!palOutput)
        {
            font = fontForHeight(bounds.height(), true, false);
        }
        else
        {
            font = QApplication::font();
            font.setBold(true);
            font.setPixelSize(
                std::clamp(
                    static_cast<int>(std::lround(bounds.height() * 0.18)),
                    84,
                    92));
            font.setStretch(88);
        }

        painter.save();
        painter.setFont(font);

        const QFontMetrics metrics(font);
        const QString text = QStringLiteral("NO VIDEO");

        const double paddingX =
            palOutput
            ? std::max(34.0, bounds.height() * 0.070)
            : std::max(18.0, bounds.height() * 0.035);

        const double paddingY =
            palOutput
            ? std::max(18.0, bounds.height() * 0.038)
            : std::max(10.0, bounds.height() * 0.020);

        const QSize textSize = metrics.size(Qt::TextSingleLine, text);

        const QRectF card(
            bounds.center().x() -
                (static_cast<double>(textSize.width()) + 2.0 * paddingX) * 0.5,
            bounds.center().y() -
                (static_cast<double>(textSize.height()) + 2.0 * paddingY) * 0.5,
            static_cast<double>(textSize.width()) + 2.0 * paddingX,
            static_cast<double>(textSize.height()) + 2.0 * paddingY);

        painter.setPen(QPen(QColor(135, 142, 146), panelBorderWidth(palOutput)));
        painter.setBrush(panelColor());
        painter.drawRoundedRect(card, 9.0, 9.0);

        painter.setPen(textColor());
        painter.drawText(
            card,
            Qt::AlignCenter,
            text);

        painter.restore();
    }

    inline QRectF vectorscopeScopeRect(
        const QRectF& bounds,
        bool palOutput = false,
        double infoWidth = 0.0)
    {
        constexpr double screenMargin = 8.0;
        constexpr double screenGap = 8.0;

        const double palGap =
            std::max(4.0, bounds.height() * 0.010);

        double availableLeft = 0.0;
        double availableTop = 0.0;
        double availableRight = 0.0;
        double availableBottom = 0.0;

        if (!palOutput)
        {
            // Desktop presentation has no video safety area. Reserve only
            // the measured information panel, one small gap and a small
            // fixed visual margin. All remaining pixels belong to the scope.
            availableLeft =
                bounds.left() +
                screenMargin +
                std::max(0.0, infoWidth) +
                screenGap;
            availableTop = bounds.top() + screenMargin;
            availableRight = bounds.right() - screenMargin;
            availableBottom = bounds.bottom() - screenMargin;
        }
        else
        {
            // bounds already IS the PAL safe area. Do not inset it again.
            // Reserve only the measured owner panel plus one small internal
            // gap; every remaining pixel belongs to the vectorscope.
            availableLeft =
                bounds.left() +
                std::max(0.0, infoWidth) + palGap;
            availableTop = bounds.top();
            availableRight = bounds.right();
            availableBottom = bounds.bottom();
        }

        const QRectF available(
            availableLeft,
            availableTop,
            std::max(1.0, availableRight - availableLeft),
            std::max(1.0, availableBottom - availableTop));

        const double size =
            std::min(available.width(), available.height());

        return QRectF(
            available.center().x() - size * 0.5,
            available.center().y() - size * 0.5,
            size,
            size);
    }

    inline QRectF vectorscopeInfoRect(
        const QRectF& bounds,
        bool palOutput = false,
        double infoWidth = 0.0)
    {
        constexpr double screenMargin = 8.0;

        if (palOutput)
        {
            // bounds already IS the PAL safe area. Owner panel starts at its
            // left/top edge and is only as wide as its measured contents.
            return QRectF(
                bounds.left(),
                bounds.top(),
                std::max(1.0, infoWidth),
                std::max(1.0, bounds.height()));
        }

        return QRectF(
            bounds.left() + screenMargin,
            bounds.top() + screenMargin,
            std::max(1.0, infoWidth),
            std::max(1.0, bounds.height() - 2.0 * screenMargin));
    }

    struct InfoRow
    {
        QString label;
        QString value;
    };

    inline QSizeF infoCardRequiredSize(
        const QVector<InfoRow>& rows,
        double referenceHeight,
        bool palOutput = false)
    {
        if (rows.isEmpty())
        {
            return {};
        }

        const QFont font = fontForHeight(referenceHeight, false, palOutput);
        const QFontMetrics metrics(font);

        const double paddingX =
            palOutput
            ? std::max(7.0, referenceHeight * 0.012)
            : std::max(9.0, referenceHeight * 0.016);

        const double paddingY =
            palOutput
            ? std::max(6.0, referenceHeight * 0.010)
            : std::max(7.0, referenceHeight * 0.012);

        const double rowGap =
            palOutput
            ? std::max(2.0, referenceHeight * 0.004)
            : std::max(3.0, referenceHeight * 0.006);

        const double labelGap =
            palOutput
            ? std::max(6.0, referenceHeight * 0.010)
            : std::max(10.0, referenceHeight * 0.018);

        int labelWidth = 0;
        int valueWidth = 0;

        for (const InfoRow& row : rows)
        {
            labelWidth =
                std::max(labelWidth, metrics.horizontalAdvance(row.label));
            valueWidth =
                std::max(valueWidth, metrics.horizontalAdvance(row.value));
        }

        const double effectiveLabelGap =
            (labelWidth > 0 && valueWidth > 0)
            ? labelGap
            : 0.0;

        const double rowHeight =
            static_cast<double>(metrics.height());

        return QSizeF(
            2.0 * paddingX +
                static_cast<double>(labelWidth) +
                effectiveLabelGap +
                static_cast<double>(valueWidth),
            2.0 * paddingY +
                static_cast<double>(rows.size()) * rowHeight +
                static_cast<double>(rows.size() - 1) * rowGap);
    }

    inline void drawOwnerPanel(
        QPainter& painter,
        const QRectF& rect,
        bool palOutput = false)
    {
        painter.save();
        painter.setPen(QPen(QColor(83, 89, 93), ownerPanelBorderWidth(palOutput)));
        painter.setBrush(ownerPanelColor());
        painter.drawRoundedRect(rect, 11.0, 11.0);
        painter.restore();
    }

    inline bool drawInfoCard(
        QPainter& painter,
        const QRectF& rect,
        const QVector<InfoRow>& rows,
        double referenceHeight,
        bool palOutput = false)
    {
        if (rows.isEmpty() || rect.width() < 1.0 || rect.height() < 1.0)
        {
            return false;
        }

        const QFont font = fontForHeight(referenceHeight, false, palOutput);
        const QFontMetrics metrics(font);

        const double paddingX =
            palOutput
            ? std::max(7.0, referenceHeight * 0.012)
            : std::max(9.0, referenceHeight * 0.016);

        const double paddingY =
            palOutput
            ? std::max(6.0, referenceHeight * 0.010)
            : std::max(7.0, referenceHeight * 0.012);

        const double rowGap =
            palOutput
            ? std::max(2.0, referenceHeight * 0.004)
            : std::max(3.0, referenceHeight * 0.006);

        const double labelGap =
            palOutput
            ? std::max(6.0, referenceHeight * 0.010)
            : std::max(10.0, referenceHeight * 0.018);

        int labelWidth = 0;
        int valueWidth = 0;

        for (const InfoRow& row : rows)
        {
            labelWidth =
                std::max(labelWidth, metrics.horizontalAdvance(row.label));
            valueWidth =
                std::max(valueWidth, metrics.horizontalAdvance(row.value));
        }

        const double effectiveLabelGap =
            (labelWidth > 0 && valueWidth > 0)
            ? labelGap
            : 0.0;

        const QSizeF required =
            infoCardRequiredSize(rows, referenceHeight, palOutput);

        if (required.width() > rect.width() ||
            required.height() > rect.height())
        {
            return false;
        }

        // Fill the available inner width. Equal-width nested cards make the
        // left side read as one coherent instrument panel instead of a set
        // of floating labels.
        const QRectF card(
            rect.left(),
            rect.top(),
            rect.width(),
            required.height());

        const double rowHeight =
            static_cast<double>(metrics.height());

        painter.save();
        painter.setPen(QPen(QColor(135, 142, 146), panelBorderWidth(palOutput)));
        painter.setBrush(panelColor());
        painter.drawRoundedRect(card, 9.0, 9.0);

        painter.setFont(font);
        painter.setPen(textColor());

        double y = card.top() + paddingY;
        for (const InfoRow& row : rows)
        {
            const QRectF labelRect(
                card.left() + paddingX,
                y,
                labelWidth,
                rowHeight);

            const QRectF valueRect(
                labelRect.right() + effectiveLabelGap,
                y,
                valueWidth,
                rowHeight);

            painter.drawText(
                labelRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                row.label);
            painter.drawText(
                valueRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                row.value);

            y += rowHeight + rowGap;
        }

        painter.restore();
        return true;
    }
}
