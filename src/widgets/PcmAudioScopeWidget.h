#pragma once

#include <QByteArray>
#include <QWidget>

class PcmAudioMeter;
class PcmAudioScale;
class StereoPhaseDisplay;
class StereoCorrelationMeter;
class QResizeEvent;

class PcmAudioScopeWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PcmAudioScopeWidget(QWidget* parent = nullptr);

    void setAudioLevels(float leftPeak, float rightPeak);
    void setStereoPcm16(const QByteArray& stereoPcm16);
    void setColorized(bool enabled);
    void setExpandedLayout(bool expanded);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    PcmAudioMeter* leftMeter_ = nullptr;
    PcmAudioMeter* rightMeter_ = nullptr;
    PcmAudioScale* scale_ = nullptr;
    StereoPhaseDisplay* phaseDisplay_ = nullptr;
    StereoCorrelationMeter* correlationMeter_ = nullptr;
    bool expandedLayout_ = false;

    void updateResponsiveLayout();
};
