#include "RenderLoadBar.h"

#include <QPainter>

#include <algorithm>

RenderLoadBar::RenderLoadBar(
    QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(18);
}

void RenderLoadBar::setMilliseconds(
    double milliseconds)
{
    milliseconds =
        std::max(
            milliseconds,
            0.0);

    if (milliseconds >
        displayMilliseconds_)
    {
        displayMilliseconds_ =
            milliseconds;
    }
    else
    {
        displayMilliseconds_ =
            displayMilliseconds_ * 0.8 +
            milliseconds * 0.2;
    }

    update();
}

void RenderLoadBar::paintEvent(
    QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    painter.fillRect(
        rect(),
        Qt::black);

    const double fraction =
        std::clamp(
            displayMilliseconds_ / 80.0,
            0.0,
            1.0);

    QColor barColor;

    if (displayMilliseconds_ > 80.0)
    {
        barColor =
            QColor(
                220,
                40,
                40);
    }
    else if (displayMilliseconds_ > 40.0)
    {
        barColor =
            QColor(
                220,
                140,
                20);
    }
    else
    {
        barColor =
            QColor(
                40,
                180,
                80);
    }

    const int barWidth =
        static_cast<int>(
            static_cast<double>(
                width()) *
            fraction);

    painter.fillRect(
        QRect(
            0,
            0,
            barWidth,
            height()),
        barColor);

    painter.setPen(
        Qt::white);

    painter.drawText(
        rect().adjusted(
            4,
            0,
            -4,
            0),
        Qt::AlignVCenter |
        Qt::AlignRight,
        QString::number(
            displayMilliseconds_,
            'f',
            1) +
        " ms");
}