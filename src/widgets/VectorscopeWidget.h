#pragma once

#include "widgets/VideoWidget.h"

#include <QSize>

class VectorscopeWidget final : public VideoWidget
{
    Q_OBJECT

public:
    explicit VectorscopeWidget(QWidget* parent = nullptr);

    [[nodiscard]] QSize renderSize() const;
};
