#pragma once

#include "settings/OpenScopeSettings.h"

#include <QPoint>
#include <QWidget>
#include <QByteArray>

#include <cstdint>

class QGridLayout;
class ScopeViewport;
class ControlWidget;
class QStackedWidget;
class PcmAudioScopeWidget;
class QString;

class ScopeWorkspace final : public QWidget
{
    Q_OBJECT

public:
    explicit ScopeWorkspace(
        QWidget* videoWidget,
        QWidget* waveformWidget,
        QWidget* vectorscopeWidget,
        const OpenScopeSettings& settings,
        QWidget* parent = nullptr);

    void setWorkspaceView(
        OpenScopeSettings::WorkspaceView view);

    void setPerformanceVisible(
        bool visible);

    void setLineNumber(
        int lineNumber);

    void setWaveformZoomFactor(
        int zoomFactor);

    void setAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

    void setCompositeInputGainState(
        bool lumaAvailable,
        bool chromaAvailable,
        int minimumHundredthsDb,
        int maximumHundredthsDb,
        int lumaHundredthsDb,
        int chromaHundredthsDb);

    void setViewFps(
        double videoOpenScopeFps,
        double videoSpoutFps,
        double waveformOpenScopeFps,
        double waveformSpoutFps,
        double vectorscopeOpenScopeFps,
        double vectorscopeSpoutFps);

    void setWssStatus(
        const QString& status);

    void setPcmAudioLevels(float leftPeak, float rightPeak);
    void setPcmAudioScopeSamples(const QByteArray& stereoPcm16);
    void setPcmAsioStatus(double sampleRate, long bufferFrames, double qMs, double targetMs,
                          double queuedMs, double fastMs, double avg3Ms, double ppm,
                          double callbackRate, std::uint64_t underruns, std::uint64_t overruns);

    void setPcmDecoderStatus(
        bool enabled,
        int activeFormat,
        bool locked,
        int validLines,
        int testedLines,
        double crcPercent,
        double bitPeriodPixels,
        double syncStartPixels,
        bool modeKnown,
        bool mode16Detected,
        bool controlValid,
        bool preEmphasis,
        int controlValidLines,
        int controlTestedLines,
        int field1ValidAudioLines,
        int field1TestedAudioLines,
        int field1ValidControlLines,
        int field1TestedControlLines,
        int field2ValidAudioLines,
        int field2TestedAudioLines,
        int field2ValidControlLines,
        int field2TestedControlLines,
        std::uint64_t reconstructedGroups,
        std::uint64_t pCorrectedGroups,
        std::uint64_t lsbPackMissingGroups,
        std::uint64_t hardUncorrectableGroups,
        double pVerifyPercent,
        double leftFrequencyHz,
        double rightFrequencyHz);

    bool isVideoMaximized() const;
    bool hasMaximizedViewport() const;

    QPoint floatingSettingsPosition() const;
    QSize floatingSettingsSize() const;
    bool hasFloatingSettingsSize() const;
    bool hasFloatingSettingsPosition() const;
    void homeFloatingSettings(const QPoint& position);

    void toggleSettingsWindow(bool forceFloating = false);

signals:
    void lineNumberChanged(int lineNumber);
    void waveformZoomChanged(int zoomFactor);
    void waveformPersistenceChanged(int persistence);
    void waveformCoreIntensityChanged(int intensity);
    void waveformCoreWidthChanged(int widthTenths);
    void vectorscopeGlowChanged(int glow);

    void waveformChromaFillIntensityChanged(int intensity);
    void waveformColorChanged(bool enabled);

    void workspaceViewChanged(
        OpenScopeSettings::WorkspaceView view);

    void performanceVisibilityChanged(bool visible);
    void floatiesHomeRequested();
    void spoutVideoEnabledChanged(bool enabled);
    void spoutWaveformEnabledChanged(bool enabled);
    void spoutVectorscopeEnabledChanged(bool enabled);
    void preventDisplaySleepChanged(bool enabled);
    void followWssAspectRatioChanged(bool enabled);
    void injectTestWss4x3Requested();
    void injectTestWss16x9Requested();

    void noiseReductionChanged(bool enabled);
    void noiseReductionIntensityChanged(int intensity);
    void antiAliasingChanged(bool enabled);
    void colorizeIllegalLuminanceChanged(bool enabled);
    void colorizeGamutErrorsChanged(bool enabled);
    void lineSelectorVisibleChanged(bool enabled);
    void safetyArea90Changed(bool enabled);
    void textSafetyArea80Changed(bool enabled);
    void lumaCompensationChanged(bool enabled);
    void lumaCompensationGainChanged(int gainHundredthsDb);

    void compositeLumaGainChanged(int gainHundredthsDb);
    void compositeChromaGainChanged(int gainHundredthsDb);
    void compositeGainCommitRequested();

    void legacyAspectRatioChanged(bool legacyEnabled);

    void videoMaximizedChanged(bool maximized);
    void exportHighResolutionPngRequested();
    void exportHighResolutionPngQuickRequested();
    void waveformRawCaptureRequested();
    void pcmDecoderEnabledChanged(bool enabled);
    void pcmDecoderModeChanged(int mode);
    void pcmAudioOutputEnabledChanged(bool enabled);
    void pcmAudioOutputBackendChanged(int backend);
    void pcmAsioDriverChanged(const QString& driverName);
    void pcmAsioBufferLoggingChanged(bool enabled);
    void pcmAsioTargetMsChanged(double milliseconds);
    void pcmAudioOutputDeviceChanged(const QString& deviceId);
    void pcmAudioBufferMsChanged(int milliseconds);
    void pcmMuteBottomTwoBitsChanged(bool enabled);
    void pcmDeEmphasisModeChanged(int mode);
    void pcmAudioScopeRequestedChanged(bool enabled);
    void pcmAudioScopeColorizedChanged(bool enabled);
    void pcmAudioScopeActiveChanged(bool active);

private:
    void showGrid();
    void showMaximized(ScopeViewport* viewport);

    void floatSettings();
    void dockSettings();
    void resizeFloatingSettings();

    QGridLayout* layout_ = nullptr;

    ScopeViewport* videoViewport_ = nullptr;
    ScopeViewport* waveformViewport_ = nullptr;
    ScopeViewport* vectorscopeViewport_ = nullptr;
    ScopeViewport* settingsViewport_ = nullptr;
    QStackedWidget* vectorscopeStack_ = nullptr;
    PcmAudioScopeWidget* pcmAudioScopeWidget_ = nullptr;

    ScopeViewport* maximizedViewport_ = nullptr;
    ControlWidget* controlWidget_ = nullptr;

    OpenScopeSettings::AspectRatio aspectRatio_ =
        OpenScopeSettings::AspectRatio::Ratio16x9;

    bool settingsFloating_ = false;
    bool pcmDecoderEnabled_ = false;
    bool pcmAudioScopeRequested_ = false;
    bool pcmAudioScopeActive_ = false;

    QPoint settingsFloatingPosition_;
    bool settingsFloatingPositionValid_ = false;
    QSize settingsFloatingSize_;
    bool settingsFloatingSizeValid_ = false;

private:
    void updatePcmAudioScopeMode();

private slots:
    void toggleMaximized(ScopeViewport* viewport);
};
