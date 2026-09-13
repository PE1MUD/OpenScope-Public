#include "widgets/VectorscopeWidget.h"

VectorscopeWidget::VectorscopeWidget(QWidget* parent)
    : VideoWidget(parent)
{
}

QSize VectorscopeWidget::renderSize() const
{
    return fitAspectSize(
        width(),
        height());
}
