#pragma once

#include <QWidget>

#include <cstdint>

#include "util/PerformanceStats.h"

class QCloseEvent;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QScrollBar;
class QSlider;

class PerformanceWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PerformanceWidget(
        QWidget* parent = nullptr);

    void setPerformanceSnapshot(
        const PerformanceSnapshot& snapshot);

    QSize sizeHint() const override;

signals:
    void visibilityChanged(
        bool visible);

protected:
    void paintEvent(
        QPaintEvent* event) override;

    void mouseMoveEvent(
        QMouseEvent* event) override;

    void mousePressEvent(
        QMouseEvent* event) override;

    void resizeEvent(
        QResizeEvent* event) override;

    void closeEvent(
        QCloseEvent* event) override;

private:
    void updateTimelineControls();
    void layoutTimelineControls();

    PerformanceSnapshot snapshot_;
    PerformanceSnapshot pinnedSnapshot_;
    int pinnedRowIndex_ = -1;
    bool hasPinnedSnapshot_ = false;

    QPushButton* pauseButton_ = nullptr;
    QSlider* timelineZoomSlider_ = nullptr;
    QScrollBar* timelineScrollBar_ = nullptr;
    QScrollBar* detailScrollBar_ = nullptr;

    bool paused_ = false;
    bool autoPauseArmed_ = false;
    double autoPauseThresholdUs_ = 0.0;
    double timelineStartUs_ = 0.0;
    double timelineSpanUs_ = 80000.0;
    int detailScrollOffset_ = 0;
};
