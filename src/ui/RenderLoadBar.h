#pragma once

#include <QWidget>

class RenderLoadBar final : public QWidget
{
    Q_OBJECT

public:
    explicit RenderLoadBar(
        QWidget* parent = nullptr);

    void setMilliseconds(
        double milliseconds);

protected:
    void paintEvent(
        QPaintEvent* event) override;

private:
    double displayMilliseconds_ = 0.0;
};