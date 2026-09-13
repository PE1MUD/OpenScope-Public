#pragma once

#include "standards/VideoStandard.h"

#include <QFont>
#include <QPaintDevice>
#include <QPainter>
#include <QRectF>

struct WaveformGraticuleLayout
{
    QRectF viewportRect;
    QRectF plotRect;
};

class WaveformGraticule
{
public:
    [[nodiscard]] WaveformGraticuleLayout layout(
        const QRectF& canvasRect,
        const QFont& baseFont,
        const QPaintDevice* device,
        double displayAspectRatio,
        bool fitAspectRatio,
        double contentScaleX,
        double contentScaleY) const;

    void draw(
        QPainter& painter,
        const QRectF& scopeRect,
        VideoStandard standard) const;

    QFont labelFont(
        const QFont& baseFont,
        double scopeHeight) const;

    double leftInset(
        const QFont& baseFont,
        const QPaintDevice* device,
        double scopeHeight) const;
};