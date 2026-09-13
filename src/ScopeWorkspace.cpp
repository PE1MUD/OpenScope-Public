#include "ScopeWorkspace.h"
#include "BuildConfig.h"
#include "widgets/WaveformWidget.h"
#include "ScopeViewport.h"
#include "widgets/ControlWidget.h"
#include "widgets/PcmAudioScopeWidget.h"

#include <QGridLayout>
#include <QPoint>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kFloatingSettingsWidth = 360;
}

ScopeWorkspace::ScopeWorkspace(
    QWidget* videoWidget,
    QWidget* waveformWidget,
    QWidget* vectorscopeWidget,
    const OpenScopeSettings& settings,
    QWidget* parent)
    : QWidget(parent)
    , aspectRatio_(
        settings.local.display.aspectRatio)
    , settingsFloatingPosition_(
        settings.local.floaties.settings.x,
        settings.local.floaties.settings.y)
    , settingsFloatingPositionValid_(
        settings.local.floaties.settings.positionValid)
    , settingsFloatingSize_(
        settings.local.floaties.settings.width,
        settings.local.floaties.settings.height)
    , settingsFloatingSizeValid_(
        settings.local.floaties.settings.sizeValid &&
        settings.local.floaties.settings.width > 0 &&
        settings.local.floaties.settings.height > 0)
{
    layout_ =
        new QGridLayout(this);

    setMinimumSize(
        768,
        432);

    layout_->setContentsMargins(
        0,
        0,
        0,
        0);

    layout_->setSpacing(4);

    // Matrix view is a strict 2x2. Content size hints must never distort
    // row/column allocation.
    layout_->setRowMinimumHeight(0, 0);
    layout_->setRowMinimumHeight(1, 0);
    layout_->setColumnMinimumWidth(0, 0);
    layout_->setColumnMinimumWidth(1, 0);

    videoViewport_ =
        new ScopeViewport(
            videoWidget,
            this);

    waveformViewport_ =
        new ScopeViewport(
            waveformWidget,
            this);

    pcmDecoderEnabled_ =
        settings.control.pcmAudio.enabled;
    pcmAudioScopeRequested_ =
        settings.control.pcmAudio.audioScopeEnabled;

    vectorscopeStack_ =
        new QStackedWidget(this);
    vectorscopeStack_->setSizePolicy(
        QSizePolicy::Ignored,
        QSizePolicy::Ignored);
    vectorscopeStack_->setMinimumSize(0, 0);

    vectorscopeWidget->setParent(vectorscopeStack_);
    pcmAudioScopeWidget_ =
        new PcmAudioScopeWidget(vectorscopeStack_);
    pcmAudioScopeWidget_->setColorized(
        settings.control.pcmAudio.audioScopeColorized);
    vectorscopeStack_->addWidget(vectorscopeWidget);
    vectorscopeStack_->addWidget(pcmAudioScopeWidget_);

    pcmAudioScopeActive_ =
        pcmDecoderEnabled_ &&
        pcmAudioScopeRequested_;
    vectorscopeStack_->setCurrentWidget(
        pcmAudioScopeActive_
            ? static_cast<QWidget*>(pcmAudioScopeWidget_)
            : vectorscopeWidget);

    vectorscopeViewport_ =
        new ScopeViewport(
            vectorscopeStack_,
            this);

    controlWidget_ =
        new ControlWidget(
            settings,
            this);

    settingsViewport_ =
        new ScopeViewport(
            controlWidget_,
            this);

    layout_->addWidget(
        videoViewport_,
        0,
        0);

    layout_->addWidget(
        waveformViewport_,
        0,
        1);

    layout_->addWidget(
        vectorscopeViewport_,
        1,
        0);

    layout_->addWidget(
        settingsViewport_,
        1,
        1);

    layout_->setRowStretch(
        0,
        1);

    layout_->setRowStretch(
        1,
        1);

    layout_->setColumnStretch(
        0,
        1);

    layout_->setColumnStretch(
        1,
        1);

    connect(
        videoViewport_,
        &ScopeViewport::doubleClicked,
        this,
        &ScopeWorkspace::toggleMaximized);

    connect(
        waveformViewport_,
        &ScopeViewport::doubleClicked,
        this,
        &ScopeWorkspace::toggleMaximized);

    connect(
        vectorscopeViewport_,
        &ScopeViewport::doubleClicked,
        this,
        &ScopeWorkspace::toggleMaximized);

    connect(
        controlWidget_,
        &ControlWidget::lineNumberChanged,
        this,
        [this, waveformWidget](int lineNumber)
        {
            if (auto* waveform =
                    qobject_cast<WaveformWidget*>(
                        waveformWidget))
            {
                waveform->clearMeasurements();
            }

            emit lineNumberChanged(
                lineNumber);
        });

    connect(
        controlWidget_,
        &ControlWidget::waveformZoomChanged,
        this,
        &ScopeWorkspace::waveformZoomChanged);

    connect(
        controlWidget_,
        &ControlWidget::waveformPersistenceChanged,
        this,
        &ScopeWorkspace::waveformPersistenceChanged);

    connect(
        controlWidget_,
        &ControlWidget::waveformCoreIntensityChanged,
        this,
        &ScopeWorkspace::waveformCoreIntensityChanged);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        connect(
            controlWidget_,
            &ControlWidget::waveformCoreWidthChanged,
            this,
            &ScopeWorkspace::waveformCoreWidthChanged);
    }

    connect(
        controlWidget_,
        &ControlWidget::vectorscopeGlowChanged,
        this,
        &ScopeWorkspace::vectorscopeGlowChanged);

    connect(
        controlWidget_,
        &ControlWidget::vintageLookChanged,
        this,
        [this](bool enabled)
        {
            emit waveformColorChanged(
                !enabled);
        });

    connect(
        controlWidget_,
        &ControlWidget::chromaRenderIntensityChanged,
        this,
        &ScopeWorkspace::waveformChromaFillIntensityChanged);

    connect(
        controlWidget_,
        &ControlWidget::performanceVisibilityChanged,
        this,
        &ScopeWorkspace::performanceVisibilityChanged);

    connect(
        controlWidget_,
        &ControlWidget::floatiesHomeRequested,
        this,
        &ScopeWorkspace::floatiesHomeRequested);

    connect(
        controlWidget_,
        &ControlWidget::spoutVideoEnabledChanged,
        this,
        &ScopeWorkspace::spoutVideoEnabledChanged);

    connect(
        controlWidget_,
        &ControlWidget::spoutWaveformEnabledChanged,
        this,
        &ScopeWorkspace::spoutWaveformEnabledChanged);

    connect(
        controlWidget_,
        &ControlWidget::spoutVectorscopeEnabledChanged,
        this,
        &ScopeWorkspace::spoutVectorscopeEnabledChanged);

    connect(
        controlWidget_,
        &ControlWidget::preventDisplaySleepChanged,
        this,
        &ScopeWorkspace::preventDisplaySleepChanged);

    connect(
        controlWidget_,
        &ControlWidget::followWssAspectRatioChanged,
        this,
        &ScopeWorkspace::followWssAspectRatioChanged);

    connect(
        controlWidget_,
        &ControlWidget::injectTestWss4x3Requested,
        this,
        &ScopeWorkspace::injectTestWss4x3Requested);

    connect(
        controlWidget_,
        &ControlWidget::injectTestWss16x9Requested,
        this,
        &ScopeWorkspace::injectTestWss16x9Requested);

    connect(
        controlWidget_,
        &ControlWidget::noiseReductionChanged,
        this,
        &ScopeWorkspace::noiseReductionChanged);

    connect(
        controlWidget_,
        &ControlWidget::antiAliasingChanged,
        this,
        &ScopeWorkspace::antiAliasingChanged);

    connect(
        controlWidget_,
        &ControlWidget::colorizeIllegalLuminanceChanged,
        this,
        &ScopeWorkspace::colorizeIllegalLuminanceChanged);

    connect(
        controlWidget_,
        &ControlWidget::colorizeGamutErrorsChanged,
        this,
        &ScopeWorkspace::colorizeGamutErrorsChanged);

    connect(
        controlWidget_,
        &ControlWidget::lineSelectorVisibleChanged,
        this,
        &ScopeWorkspace::lineSelectorVisibleChanged);

    connect(
        controlWidget_,
        &ControlWidget::safetyArea90Changed,
        this,
        &ScopeWorkspace::safetyArea90Changed);

    connect(
        controlWidget_,
        &ControlWidget::textSafetyArea80Changed,
        this,
        &ScopeWorkspace::textSafetyArea80Changed);

    connect(
        controlWidget_,
        &ControlWidget::noiseReductionIntensityChanged,
        this,
        &ScopeWorkspace::noiseReductionIntensityChanged);

    connect(
        controlWidget_,
        &ControlWidget::lumaCompensationChanged,
        this,
        &ScopeWorkspace::lumaCompensationChanged);

    connect(
        controlWidget_,
        &ControlWidget::lumaCompensationGainChanged,
        this,
        &ScopeWorkspace::lumaCompensationGainChanged);

    connect(
        controlWidget_,
        &ControlWidget::compositeLumaGainChanged,
        this,
        &ScopeWorkspace::compositeLumaGainChanged);

    connect(
        controlWidget_,
        &ControlWidget::compositeChromaGainChanged,
        this,
        &ScopeWorkspace::compositeChromaGainChanged);

    connect(
        controlWidget_,
        &ControlWidget::compositeGainCommitRequested,
        this,
        &ScopeWorkspace::compositeGainCommitRequested);

    connect(
        controlWidget_,
        &ControlWidget::legacyAspectRatioChanged,
        this,
        &ScopeWorkspace::legacyAspectRatioChanged);

    connect(
        controlWidget_,
        &ControlWidget::exportHighResolutionPngRequested,
        this,
        &ScopeWorkspace::exportHighResolutionPngRequested);

    connect(
        controlWidget_,
        &ControlWidget::exportHighResolutionPngQuickRequested,
        this,
        &ScopeWorkspace::exportHighResolutionPngQuickRequested);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        connect(
            controlWidget_,
            &ControlWidget::waveformRawCaptureRequested,
            this,
            &ScopeWorkspace::waveformRawCaptureRequested);
    }

    connect(
        controlWidget_,
        &ControlWidget::pcmDecoderEnabledChanged,
        this,
        [this](bool enabled)
        {
            pcmDecoderEnabled_ = enabled;
            updatePcmAudioScopeMode();
            emit pcmDecoderEnabledChanged(enabled);
        });

    connect(
        controlWidget_,
        &ControlWidget::pcmDecoderModeChanged,
        this,
        [this](int mode)
        {
            emit pcmDecoderModeChanged(mode);
        });

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioScopeEnabledChanged,
        this,
        [this](bool enabled)
        {
            pcmAudioScopeRequested_ = enabled;
            updatePcmAudioScopeMode();
            emit pcmAudioScopeRequestedChanged(enabled);
        });

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioScopeColorizedChanged,
        this,
        [this](bool enabled)
        {
            if (pcmAudioScopeWidget_ != nullptr)
                pcmAudioScopeWidget_->setColorized(enabled);
            emit pcmAudioScopeColorizedChanged(enabled);
        });

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioOutputEnabledChanged,
        this,
        &ScopeWorkspace::pcmAudioOutputEnabledChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioOutputBackendChanged,
        this,
        &ScopeWorkspace::pcmAudioOutputBackendChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAsioDriverChanged,
        this,
        &ScopeWorkspace::pcmAsioDriverChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAsioBufferLoggingChanged,
        this,
        &ScopeWorkspace::pcmAsioBufferLoggingChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAsioTargetMsChanged,
        this,
        &ScopeWorkspace::pcmAsioTargetMsChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioOutputDeviceChanged,
        this,
        &ScopeWorkspace::pcmAudioOutputDeviceChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmAudioBufferMsChanged,
        this,
        &ScopeWorkspace::pcmAudioBufferMsChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmMuteBottomTwoBitsChanged,
        this,
        &ScopeWorkspace::pcmMuteBottomTwoBitsChanged);

    connect(
        controlWidget_,
        &ControlWidget::pcmDeEmphasisModeChanged,
        this,
        &ScopeWorkspace::pcmDeEmphasisModeChanged);
}

void ScopeWorkspace::setPcmAudioLevels(float leftPeak, float rightPeak)
{
    if (controlWidget_ != nullptr)
        controlWidget_->setPcmAudioLevels(leftPeak, rightPeak);
    if (pcmAudioScopeWidget_ != nullptr)
        pcmAudioScopeWidget_->setAudioLevels(leftPeak, rightPeak);
}

void ScopeWorkspace::setPcmAudioScopeSamples(const QByteArray& stereoPcm16)
{
    if (pcmAudioScopeWidget_ != nullptr && pcmAudioScopeActive_)
        pcmAudioScopeWidget_->setStereoPcm16(stereoPcm16);
}

void ScopeWorkspace::setPcmAsioStatus(
    double sampleRate,
    long bufferFrames,
    double qMs,
    double targetMs,
    double queuedMs,
    double fastMs,
    double avg3Ms,
    double ppm,
    double callbackRate,
    std::uint64_t underruns,
    std::uint64_t overruns)
{
    if (controlWidget_ != nullptr)
        controlWidget_->setPcmAsioStatus(sampleRate, bufferFrames, qMs, targetMs,
                                         queuedMs, fastMs, avg3Ms, ppm,
                                         callbackRate, underruns, overruns);
}

void ScopeWorkspace::updatePcmAudioScopeMode()
{
    const bool active =
        pcmDecoderEnabled_ &&
        pcmAudioScopeRequested_;

    if (vectorscopeStack_ != nullptr)
    {
        vectorscopeStack_->setCurrentIndex(active ? 1 : 0);
    }

    if (active == pcmAudioScopeActive_)
        return;

    pcmAudioScopeActive_ = active;
    emit pcmAudioScopeActiveChanged(active);
}

void ScopeWorkspace::setPcmDecoderStatus(
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
    double rightFrequencyHz)
{
    controlWidget_->setPcmDecoderStatus(
        enabled,
        activeFormat,
        locked,
        validLines,
        testedLines,
        crcPercent,
        bitPeriodPixels,
        syncStartPixels,
        modeKnown,
        mode16Detected,
        controlValid,
        preEmphasis,
        controlValidLines,
        controlTestedLines,
        field1ValidAudioLines,
        field1TestedAudioLines,
        field1ValidControlLines,
        field1TestedControlLines,
        field2ValidAudioLines,
        field2TestedAudioLines,
        field2ValidControlLines,
        field2TestedControlLines,
        reconstructedGroups,
        pCorrectedGroups,
        lsbPackMissingGroups,
        hardUncorrectableGroups,
        pVerifyPercent,
        leftFrequencyHz,
        rightFrequencyHz);
}


void ScopeWorkspace::setWssStatus(
    const QString& status)
{
    controlWidget_->setWssStatus(
        status);
}


void ScopeWorkspace::setViewFps(
    double videoOpenScopeFps,
    double videoSpoutFps,
    double waveformOpenScopeFps,
    double waveformSpoutFps,
    double vectorscopeOpenScopeFps,
    double vectorscopeSpoutFps)
{
    if (controlWidget_ == nullptr)
    {
        return;
    }

    controlWidget_->setViewFps(
        videoOpenScopeFps,
        videoSpoutFps,
        waveformOpenScopeFps,
        waveformSpoutFps,
        vectorscopeOpenScopeFps,
        vectorscopeSpoutFps);
}


void ScopeWorkspace::setCompositeInputGainState(
    bool lumaAvailable,
    bool chromaAvailable,
    int minimumHundredthsDb,
    int maximumHundredthsDb,
    int lumaHundredthsDb,
    int chromaHundredthsDb)
{
    if (controlWidget_ == nullptr)
    {
        return;
    }

    controlWidget_->setCompositeInputGainState(
        lumaAvailable,
        chromaAvailable,
        minimumHundredthsDb,
        maximumHundredthsDb,
        lumaHundredthsDb,
        chromaHundredthsDb);
}

void ScopeWorkspace::toggleMaximized(
    ScopeViewport* viewport)
{
    if (maximizedViewport_ == viewport)
    {
        const bool leavingVideo =
            viewport == videoViewport_;

        showGrid();

        if (leavingVideo)
        {
            emit videoMaximizedChanged(false);
        }

        emit workspaceViewChanged(
            OpenScopeSettings::WorkspaceView::Matrix);

        return;
    }

    const bool videoWasMaximized =
        maximizedViewport_ == videoViewport_;

    showMaximized(viewport);

    const bool videoIsMaximized =
        viewport == videoViewport_;

    if (videoWasMaximized != videoIsMaximized)
    {
        emit videoMaximizedChanged(
            videoIsMaximized);
    }

    if (viewport == videoViewport_)
    {
        emit workspaceViewChanged(
            OpenScopeSettings::WorkspaceView::Video);
    }
    else if (viewport == waveformViewport_)
    {
        emit workspaceViewChanged(
            OpenScopeSettings::WorkspaceView::Waveform);
    }
    else if (viewport == vectorscopeViewport_)
    {
        emit workspaceViewChanged(
            OpenScopeSettings::WorkspaceView::Vectorscope);
    }
}

void ScopeWorkspace::showMaximized(
    ScopeViewport* viewport)
{
    controlWidget_->setHelpTabVisible(
        false);

    videoViewport_->hide();
    waveformViewport_->hide();
    vectorscopeViewport_->hide();

    // Settings used to be forced into a floating tool window whenever a
    // scope viewport was maximized.  On a single-monitor setup that covers
    // the instrument the user just asked to see.  Keep an already-open
    // floating Settings window untouched, but leave docked Settings hidden
    // until the user explicitly requests it with Config (F2).
    if (!settingsFloating_)
    {
        settingsViewport_->hide();
    }

    layout_->removeWidget(
        viewport);

    layout_->addWidget(
        viewport,
        0,
        0,
        2,
        2);

    viewport->show();
    viewport->focusContent();

    maximizedViewport_ =
        viewport;

    if (pcmAudioScopeWidget_ != nullptr)
    {
        pcmAudioScopeWidget_->setExpandedLayout(
            viewport == vectorscopeViewport_);
    }
}

void ScopeWorkspace::showGrid()
{
    controlWidget_->setHelpTabVisible(
        true);

    dockSettings();

    layout_->removeWidget(
        videoViewport_);

    layout_->removeWidget(
        waveformViewport_);

    layout_->removeWidget(
        vectorscopeViewport_);

    layout_->addWidget(
        videoViewport_,
        0,
        0);

    layout_->addWidget(
        waveformViewport_,
        0,
        1);

    layout_->addWidget(
        vectorscopeViewport_,
        1,
        0);

    layout_->addWidget(
        settingsViewport_,
        1,
        1);

    videoViewport_->show();
    waveformViewport_->show();
    vectorscopeViewport_->show();
    settingsViewport_->show();

    maximizedViewport_ =
        nullptr;

    if (pcmAudioScopeWidget_ != nullptr)
        pcmAudioScopeWidget_->setExpandedLayout(false);
}


void ScopeWorkspace::toggleSettingsWindow(bool forceFloating)
{
    if (settingsViewport_ == nullptr)
    {
        return;
    }

    const bool needFloatingWindow =
        forceFloating ||
        maximizedViewport_ != nullptr;

    // In Matrix view Settings already occupies its normal fourth quadrant.
    // Config (F2) simply brings that existing pane to the foreground.
    if (!needFloatingWindow && !settingsFloating_)
    {
        settingsViewport_->show();
        settingsViewport_->raise();
        settingsViewport_->focusContent();
        return;
    }

    if (!settingsFloating_)
    {
        floatSettings();
        return;
    }

    if (settingsViewport_->isVisible())
    {
        // Closing/hiding the floating window deliberately leaves it detached.
        // A later F2 can show the same window again without disturbing the
        // maximized instrument.  showGrid() will dock it again normally.
        settingsViewport_->hide();
        return;
    }

    settingsViewport_->show();
    settingsViewport_->raise();
    settingsViewport_->activateWindow();
    settingsViewport_->focusContent();
}

void ScopeWorkspace::floatSettings()
{
    if (settingsFloating_)
    {
        settingsViewport_->show();
        settingsViewport_->raise();
        return;
    }

    const QPoint floatingPosition =
        settingsFloatingPositionValid_
        ? settingsFloatingPosition_
        : mapToGlobal(
            QPoint(
                (std::max)(
                    width() -
                    kFloatingSettingsWidth -
                    20,
                    20),
                20));

    QWindow* mainWindowHandle = nullptr;

    if (QWidget* const mainWindow = window())
    {
        // Force creation of the native main-window handle before
        // the settings viewport becomes a top-level tool window.
        mainWindow->winId();
        mainWindowHandle =
            mainWindow->windowHandle();
    }

    layout_->removeWidget(
        settingsViewport_);

    settingsViewport_->hide();
    settingsViewport_->setParent(nullptr);

    settingsViewport_->setWindowFlags(
        Qt::Tool |
        Qt::CustomizeWindowHint |
        Qt::WindowTitleHint);

    settingsViewport_->setWindowTitle(
        "OpenScope Settings");

    // Force creation of the tool-window handle so we can make it
    // transient for the OpenScope main window. This keeps Settings
    // above OpenScope without making it system-wide always-on-top.
    settingsViewport_->winId();

    if (QWindow* const settingsWindowHandle =
        settingsViewport_->windowHandle())
    {
        settingsWindowHandle->setTransientParent(
            mainWindowHandle);
    }

    resizeFloatingSettings();
    settingsViewport_->move(
        floatingPosition);

    settingsFloatingPosition_ =
        floatingPosition;

    settingsFloatingPositionValid_ =
        true;

    settingsFloating_ = true;
    settingsViewport_->show();
    settingsViewport_->raise();
}

void ScopeWorkspace::dockSettings()
{
    if (!settingsFloating_)
    {
        return;
    }

    settingsFloatingPosition_ =
        settingsViewport_->pos();

    settingsFloatingPositionValid_ =
        true;

    settingsFloatingSize_ =
        settingsViewport_->size();
    settingsFloatingSizeValid_ =
        settingsFloatingSize_.width() > 0 &&
        settingsFloatingSize_.height() > 0;

    settingsViewport_->hide();
    settingsViewport_->setParent(this);
    settingsViewport_->setWindowFlags(
        Qt::Widget);

    settingsFloating_ = false;
}

void ScopeWorkspace::resizeFloatingSettings()
{
    if (settingsFloatingSizeValid_)
    {
        settingsViewport_->resize(
            settingsFloatingSize_);
        return;
    }

    const double aspectRatio =
        OpenScopeSettings::aspectRatioValue(
            aspectRatio_);

    const int height =
        static_cast<int>(
            std::lround(
                static_cast<double>(
                    kFloatingSettingsWidth) /
                aspectRatio));

    settingsViewport_->resize(
        kFloatingSettingsWidth,
        height);
}

QSize ScopeWorkspace::floatingSettingsSize() const
{
    if (settingsFloating_ && settingsViewport_ != nullptr)
    {
        return settingsViewport_->size();
    }

    return settingsFloatingSize_;
}

bool ScopeWorkspace::hasFloatingSettingsSize() const
{
    return settingsFloatingSizeValid_ ||
        (settingsFloating_ && settingsViewport_ != nullptr);
}

QPoint ScopeWorkspace::floatingSettingsPosition() const
{
    if (settingsFloating_)
    {
        return settingsViewport_->pos();
    }

    return settingsFloatingPosition_;
}

void ScopeWorkspace::homeFloatingSettings(
    const QPoint& position)
{
    settingsFloatingPosition_ =
        position;

    settingsFloatingPositionValid_ =
        true;

    if (settingsFloating_ &&
        settingsViewport_ != nullptr)
    {
        settingsViewport_->move(
            position);
    }
}

bool ScopeWorkspace::hasFloatingSettingsPosition() const
{
    return
        settingsFloating_ ||
        settingsFloatingPositionValid_;
}

void ScopeWorkspace::setWorkspaceView(
    OpenScopeSettings::WorkspaceView view)
{
    const bool videoWasMaximized =
        maximizedViewport_ == videoViewport_;

    switch (view)
    {
    case OpenScopeSettings::WorkspaceView::Video:
        showMaximized(videoViewport_);
        break;

    case OpenScopeSettings::WorkspaceView::Waveform:
        showMaximized(waveformViewport_);
        break;

    case OpenScopeSettings::WorkspaceView::Vectorscope:
        showMaximized(vectorscopeViewport_);
        break;

    case OpenScopeSettings::WorkspaceView::Matrix:
    case OpenScopeSettings::WorkspaceView::Headless:
        showGrid();
        break;
    }

    const bool videoIsMaximized =
        maximizedViewport_ == videoViewport_;

    if (videoWasMaximized != videoIsMaximized)
    {
        emit videoMaximizedChanged(
            videoIsMaximized);
    }
}

void ScopeWorkspace::setLineNumber(
    int lineNumber)
{
    controlWidget_->setLineNumber(
        lineNumber);
}

void ScopeWorkspace::setWaveformZoomFactor(
    int zoomFactor)
{
    controlWidget_->setWaveformZoomFactor(
        zoomFactor);
}

void ScopeWorkspace::setPerformanceVisible(
    bool visible)
{
    controlWidget_->setPerformanceVisible(
        visible);
}

void ScopeWorkspace::setAspectRatio(
    OpenScopeSettings::AspectRatio aspectRatio)
{
    if (aspectRatio_ == aspectRatio)
    {
        return;
    }

    aspectRatio_ = aspectRatio;

    controlWidget_->setAspectRatio(
        aspectRatio);

    // A floating Settings window is user-resizable.  Do not overwrite its
    // remembered/manual size when the display aspect ratio changes.
}

bool ScopeWorkspace::isVideoMaximized() const
{
    return
        maximizedViewport_ ==
        videoViewport_;
}

bool ScopeWorkspace::hasMaximizedViewport() const
{
    return maximizedViewport_ != nullptr;
}
