#include "TestPatternGenerator.h"

#include <QColor>
#include <QPainter>

namespace {

    void drawBars(QPainter& painter, const QRect& area)
    {
        static const QColor colors[] = {
            QColor(191, 191, 191),
            QColor(191, 191,   0),
            QColor(0, 191, 191),
            QColor(0, 191,   0),
            QColor(191,   0, 191),
            QColor(191,   0,   0),
            QColor(0,   0, 191)
        };

        constexpr int barCount =
            static_cast<int>(sizeof(colors) / sizeof(colors[0]));

        for (int bar = 0; bar < barCount; ++bar) {
            const int left = area.left() + area.width() * bar / barCount;
            const int right =
                area.left() + area.width() * (bar + 1) / barCount;

            painter.fillRect(
                QRect(left, area.top(), right - left, area.height()),
                colors[bar]);
        }
    }

    void drawGreySteps(QPainter& painter, const QRect& area)
    {
        constexpr int stepCount = 11;

        for (int step = 0; step < stepCount; ++step) {
            const int left = area.left() + area.width() * step / stepCount;
            const int right =
                area.left() + area.width() * (step + 1) / stepCount;

            const int level = 255 * step / (stepCount - 1);

            painter.fillRect(
                QRect(left, area.top(), right - left, area.height()),
                QColor(level, level, level));
        }
    }

    void drawPluge(QPainter& painter, const QRect& area)
    {
        painter.fillRect(area, Qt::black);

        constexpr int levels[] = {
            0,
            8,
            16,
            32
        };

        constexpr int barCount =
            static_cast<int>(sizeof(levels) / sizeof(levels[0]));

        const int barWidth = area.width() / 10;
        const int totalWidth = barWidth * barCount;
        const int left = area.center().x() - totalWidth / 2;

        const int barHeight = area.height() * 3 / 4;
        const int top = area.center().y() - barHeight / 2;

        for (int bar = 0; bar < barCount; ++bar) {
            const int level = levels[bar];

            painter.fillRect(
                QRect(
                    left + bar * barWidth,
                    top,
                    barWidth,
                    barHeight),
                QColor(level, level, level));
        }
    }

}

QImage TestPatternGenerator::generate(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return {};
    }

    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::black);

    QPainter painter(&image);

    const int margin = qMax(1, height / 50);
    const QRect content =
        image.rect().adjusted(margin, margin, -margin, -margin);

    const int barsHeight = content.height() * 2 / 3;
    const int greyHeight = content.height() / 8;
    const int plugeHeight = content.height() - barsHeight - greyHeight;

    const QRect bars(
        content.left(),
        content.top(),
        content.width(),
        barsHeight);

    const QRect grey(
        content.left(),
        bars.bottom() + 1,
        content.width(),
        greyHeight);

    const QRect pluge(
        content.left(),
        grey.bottom() + 1,
        content.width(),
        plugeHeight);

    drawBars(painter, bars);
    drawGreySteps(painter, grey);
    drawPluge(painter, pluge);
        
    return image;
}