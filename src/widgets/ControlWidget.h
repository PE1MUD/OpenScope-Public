#pragma once

#include "settings/OpenScopeSettings.h"

#include <QWidget>

#include <cstdint>

class QCheckBox;
class QButtonGroup;
class QToolButton;
class QSpinBox;
class QTabWidget;
class QSlider;
class QLabel;
class QResizeEvent;
class QString;
class AsioBufferGraphWidget;

class ControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlWidget(
        const OpenScopeSettings& settings,
        QWidget* parent = nullptr);

    void setPerformanceVisible(
        bool visible);

    void setLineNumber(
        int lineNumber);

    void setWaveformZoomFactor(
        int zoomFactor);

    void setAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

    void setHelpTabVisible(
        bool visible);

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

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void lineNumberChanged(int lineNumber);
    void waveformZoomChanged(int zoomFactor);
    void waveformPersistenceChanged(int persistence);
    void waveformCoreIntensityChanged(int intensity);
    void waveformCoreWidthChanged(int widthTenths);
    void vectorscopeGlowChanged(int glow);

    void vintageLookChanged(bool enabled);
    void chromaRenderIntensityChanged(int intensity);

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

    void performanceVisibilityChanged(bool visible);
    void floatiesHomeRequested();
    void spoutVideoEnabledChanged(bool enabled);
    void spoutWaveformEnabledChanged(bool enabled);
    void spoutVectorscopeEnabledChanged(bool enabled);
    void preventDisplaySleepChanged(bool enabled);
    void followWssAspectRatioChanged(bool enabled);
    void injectTestWss4x3Requested();
    void injectTestWss16x9Requested();
    void legacyAspectRatioChanged(bool legacyEnabled);
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
    void pcmAudioScopeEnabledChanged(bool enabled);
    void pcmAudioScopeColorizedChanged(bool enabled);

private:
    void updateBrandingLayout();

    QCheckBox* performanceCheckBox_ = nullptr;
    QCheckBox* legacyAspectRatioCheckBox_ = nullptr;
    QButtonGroup* waveformZoomButtonGroup_ = nullptr;
    QToolButton* waveformZoom1Button_ = nullptr;
    QToolButton* waveformZoom5Button_ = nullptr;
    QToolButton* waveformZoom10Button_ = nullptr;
    QSpinBox* lineSelector_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QSlider* compositeLumaGainSlider_ = nullptr;
    QSlider* compositeChromaGainSlider_ = nullptr;
    QLabel* compositeGainStatusLabel_ = nullptr;
    QLabel* videoOpenScopeFpsLabel_ = nullptr;
    QLabel* videoSpoutFpsLabel_ = nullptr;
    QLabel* waveformOpenScopeFpsLabel_ = nullptr;
    QLabel* waveformSpoutFpsLabel_ = nullptr;
    QLabel* vectorscopeOpenScopeFpsLabel_ = nullptr;
    QLabel* vectorscopeSpoutFpsLabel_ = nullptr;
    QLabel* wssStatusLabel_ = nullptr;
    QLabel* pcmLockStatusLabel_ = nullptr;
    QLabel* pcmTransportQualityLabel_ = nullptr;
    QLabel* pcmTransportLinesLabel_ = nullptr;
    QLabel* pcmModeStatusLabel_ = nullptr;
    QLabel* pcmPreEmphasisStatusLabel_ = nullptr;
    QLabel* pcmControlStatusLabel_ = nullptr;
    QLabel* pcmCrcStatusLabel_ = nullptr;
    QLabel* pcmLinesStatusLabel_ = nullptr;
    QLabel* pcmGeometryStatusLabel_ = nullptr;
    QLabel* pcmPStatusLabel_ = nullptr;
    QLabel* pcmAsioDriverStatusLabel_ = nullptr;
    QLabel* pcmAsioBufferStatusLabel_ = nullptr;
    QLabel* pcmAsioRateStatusLabel_ = nullptr;
    QLabel* pcmAsioCounterStatusLabel_ = nullptr;
    QSpinBox* pcmAsioTargetSpin_ = nullptr;
    AsioBufferGraphWidget* pcmAsioGraph_ = nullptr;
    QCheckBox* pcmAudioScopeCheckBox_ = nullptr;
    QCheckBox* pcmAudioScopeColorizeCheckBox_ = nullptr;
    int pcmDecoderSelectionMode_ = 0;
    int pcmDeEmphasisMode_ = 0;
    bool pcmResolvedHamMode_ = false;
    QLabel* pcmDecodeStatusLabel_ = nullptr;
    QLabel* pcmFrequencyStatusLabel_ = nullptr;
    QLabel* aboutLogoLabel_ = nullptr;
    QLabel* cornerLogoLabel_ = nullptr;
    int helpTabIndex_ = -1;
    int aboutTabIndex_ = -1;
};
