#pragma once
#include "standards/VideoStandard.h"
#include <QPainter>
#include <QRectF>

class VectorscopeGraticule
{
public:
    void draw(
        QPainter& painter,
        const QRectF& scopeRect,
        double lineScale = 1.0,
        double labelScale = 1.0) const;

    void setLineWidth(double width);
    void setScale(double scale);
    void setChromaMagnitude(double normalizedMagnitude);

private:
    void drawAxes(
        QPainter& painter,
        const QPointF& center,
        double radius,
        double lineScale,
        double labelScale) const;

    VideoStandard videoStandard_ =
        VideoStandard::pal625();

    double lineWidth_ = 1.0;
    double scale_ = 1.0;
    double chromaMagnitude_ = 0.0;
};