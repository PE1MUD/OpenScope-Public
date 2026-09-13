#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QWidget>

class QPaintEvent;
class QTimer;

// Compact/full RTW-style stereo meter element used by the PCM Audio Scope.
// The number of LED segments is selected from the actual available X pixels:
// compact views use 101 segments, roomy views use the full 201-segment front.
class PcmAudioMeter final : public QWidget
{
    Q_OBJECT

public:
    explicit PcmAudioMeter(QWidget* parent = nullptr);

    void setInputPeak(float linearPeak);
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void animateBallistics();
    static double linearToDb(double linear);

    QTimer* animationTimer_ = nullptr;
    QElapsedTimer elapsed_;
    double targetDb_ = -60.0;
    double displayedDb_ = -60.0;
    double peakHoldDb_ = -60.0;
    double peakHoldSecondsRemaining_ = 0.0;
};

class PcmAudioScale final : public QWidget
{
    Q_OBJECT

public:
    explicit PcmAudioScale(QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
