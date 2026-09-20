#include "MainWindow.h"
#include "BuildConfig.h"
#include "Version.h"
#include "ScopeWorkspace.h"
#include "VideoEngine.h"
#include "widgets/VideoWidget.h"
#include "widgets/WaveformWidget.h"
#include "widgets/YSpectrumWindow.h"
#include "DeckLinkProbe.h"
#include "widgets/VectorscopeWidget.h"
#include "settings/SettingsService.h"
#include "widgets/PerformanceWidget.h"
#include "standards/VideoStandard.h"
#include "output/SpoutOutput.h"
#include "sources/philips/PhilipsPatternRomSource.h"
#include "video/Yuv444Frame.h"
#include "diagnostics/TraceLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmsystem.h>

#include <QWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QTimer>
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>
#include <QImageReader>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

#include <memory>
#include <QFileDialog>
#include <QMessageBox>
#include <QImage>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QShortcut>
#include <QKeySequence>
#include <QKeyEvent>
#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>

namespace
{
constexpr int kImagePalWidth = 720;
constexpr int kImagePalHeight = 576;

// PAL D1 pixels are slightly wider than square pixels when shown as 4:3.
// (4/3) / (720/576) = 16/15.
constexpr double kPalPixelAspectRatio = 16.0 / 15.0;

std::uint16_t expand8To16Image(int value)
{
    return static_cast<std::uint16_t>(
        std::clamp(value, 0, 255) * 257);
}

QImage imageToPalRaster(const QImage& source)
{
    // Native PAL raster: keep every pixel exactly as supplied.
    if (source.width() == kImagePalWidth &&
        source.height() == kImagePalHeight)
    {
        return source.convertToFormat(QImage::Format_RGB888);
    }

    // OpenScope still-image export:
    // video raster is exported at 4x linear resolution to retain detail.
    // Reduce exactly 4:1, without smoothing and without any extra aspect
    // correction. This preserves the round-trip raster geometry and avoids
    // low-pass filtering of multiburst/high-frequency detail.
    if (source.width() == kImagePalWidth * 4 &&
        source.height() == kImagePalHeight * 4)
    {
        return source.scaled(
            kImagePalWidth,
            kImagePalHeight,
            Qt::IgnoreAspectRatio,
            Qt::FastTransformation)
            .convertToFormat(QImage::Format_RGB888);
    }

    // General still image:
    // fit directly into PAL raster space, accounting for PAL's 16:15
    // display pixel aspect. There is only ONE resize operation.
    const double sourceAspect =
        static_cast<double>(source.width()) /
        static_cast<double>(source.height());

    const double targetRasterAspect =
        sourceAspect / kPalPixelAspectRatio;

    int targetWidth = kImagePalWidth;
    int targetHeight =
        static_cast<int>(
            std::lround(
                static_cast<double>(targetWidth) /
                targetRasterAspect));

    if (targetHeight > kImagePalHeight)
    {
        targetHeight = kImagePalHeight;
        targetWidth =
            static_cast<int>(
                std::lround(
                    static_cast<double>(targetHeight) *
                    targetRasterAspect));
    }

    targetWidth = std::clamp(targetWidth, 1, kImagePalWidth);
    targetHeight = std::clamp(targetHeight, 1, kImagePalHeight);

    QImage palRaster(
        kImagePalWidth,
        kImagePalHeight,
        QImage::Format_RGB888);
    palRaster.fill(Qt::black);

    const QImage scaled =
        source.scaled(
            targetWidth,
            targetHeight,
            Qt::IgnoreAspectRatio,
            Qt::FastTransformation)
            .convertToFormat(QImage::Format_RGB888);

    QPainter painter(&palRaster);
    const QPoint topLeft(
        (kImagePalWidth - targetWidth) / 2,
        (kImagePalHeight - targetHeight) / 2);
    painter.drawImage(topLeft, scaled);

    return palRaster;
}

bool imageToPalYuv444(
    const QString& fileName,
    Yuv444Frame& destination,
    QString* errorMessage,
    QSize* sourceImageSize)
{
    QImageReader reader(fileName);
    reader.setAutoTransform(true);

    QImage source = reader.read();
    if (source.isNull())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage =
                QObject::tr("Could not open image:\n%1\n\n%2")
                    .arg(fileName, reader.errorString());
        }
        return false;
    }

    if (sourceImageSize != nullptr)
    {
        *sourceImageSize = source.size();
    }

    source = source.convertToFormat(QImage::Format_RGB888);
    const QImage palRgb = imageToPalRaster(source);

    destination.resize(kImagePalWidth, kImagePalHeight);
    destination.sampleClockHz = 13'500'000.0;
    destination.inputSignalValid = true;

    for (int y = 0; y < kImagePalHeight; ++y)
    {
        const uchar* line = palRgb.constScanLine(y);
        const std::size_t rowOffset =
            static_cast<std::size_t>(y) * kImagePalWidth;

        for (int x = 0; x < kImagePalWidth; ++x)
        {
            const int r = line[x * 3 + 0];
            const int g = line[x * 3 + 1];
            const int b = line[x * 3 + 2];

            // Existing BT.601 studio-range RGB -> YCbCr conversion.
            // Deliberately unchanged.
            const int yy =
                std::clamp(
                    ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16,
                    16,
                    235);
            const int uu =
                std::clamp(
                    ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128,
                    16,
                    240);
            const int vv =
                std::clamp(
                    ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128,
                    16,
                    240);

            const std::size_t index =
                rowOffset + static_cast<std::size_t>(x);
            destination.y[index] = expand8To16Image(yy);
            destination.u[index] = expand8To16Image(uu);
            destination.v[index] = expand8To16Image(vv);
        }
    }

    return true;
}

QSize devicePixelRenderSize(
    const QWidget* widget,
    int logicalWidth,
    int logicalHeight)
{
    const double devicePixelRatio =
        widget != nullptr
            ? widget->devicePixelRatioF()
            : 1.0;

    return QSize(
        (std::max)(
            1,
            static_cast<int>(
                std::lround(
                    static_cast<double>(logicalWidth) *
                    devicePixelRatio))),
        (std::max)(
            1,
            static_cast<int>(
                std::lround(
                    static_cast<double>(logicalHeight) *
                    devicePixelRatio))));
}

class WaveformVideoPreview final : public QWidget
{
public:
    explicit WaveformVideoPreview(QWidget* parent = nullptr)
        : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Waveform Video Out");

        constexpr VideoStandard videoStandard =
            VideoStandard::pal625();

        setFixedSize(
            videoStandard.outputWidth,
            videoStandard.outputHeight);
    }

    void setImage(const QImage& image)
    {
        image_ = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);

        if (image_.isNull())
        {
            return;
        }

        // 1:1 inspection of the actual video-out raster.
        painter.drawImage(
            QPoint(0, 0),
            image_);
    }

private:
    QImage image_;
};
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , videoWidget_(new VideoWidget)
    , videoEngine_(new VideoEngine(this))
{
#ifdef Q_OS_WIN
    // OpenScope is a real-time-ish monitoring application. Keep 1 ms timer
    // resolution requested for the complete lifetime of the process,
    // independent of focus/visibility/minimized state.
    timeBeginPeriod(1);

    // Windows 11 may otherwise ignore a process' timer-resolution request when
    // all of its windows are fully occluded/minimized. Explicitly opt OpenScope
    // out of that power-throttling behaviour, and also keep execution-speed
    // throttling disabled.
    PROCESS_POWER_THROTTLING_STATE powerState{};
    powerState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    powerState.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#endif
        ;
    powerState.StateMask = 0;
    SetProcessInformation(
        GetCurrentProcess(),
        ProcessPowerThrottling,
        &powerState,
        sizeof(powerState));
#endif
    if (QStringLiteral(OPENSCOPE_DELTA) == QStringLiteral("0"))
    {
        setWindowTitle("OpenScope V" OPENSCOPE_VERSION);
    }
    else
    {
        setWindowTitle("OpenScope V" OPENSCOPE_VERSION " - Delta " OPENSCOPE_DELTA);
    }

    // Catch plain F at the QApplication level so the spectrum shortcut
    // also works while focus is inside separate Qt instrument/tool windows.
    QCoreApplication::instance()->installEventFilter(this);

    SetThreadPriority(
        GetCurrentThread(),
        THREAD_PRIORITY_HIGHEST);

    waveformWidget_ =
        new WaveformWidget;

    ySpectrumWindow_ =
        new YSpectrumWindow(this);

    ySpectrumWindow_->setSnrMeasurementEnabled(true);
    waveformWidget_->setSnrMeasurementEnabled(true);

    vectorscopeWidget_ =
        new VectorscopeWidget;

    settingsService_ =
        new SettingsService(this);

    const OpenScopeSettings& initialSettings =
        settingsService_->settings();

    pcmAudioScopeActive_ =
        initialSettings.control.pcmAudio.enabled &&
        initialSettings.control.pcmAudio.audioScopeEnabled;
    videoEngine_->setVectorscopeSuppressedForAudioScope(
        pcmAudioScopeActive_);

    videoWidget_->setSafetyAreas(
        initialSettings.local.display.safetyArea90,
        initialSettings.local.display.textSafetyArea80);
    videoEngine_->setVideoLineHighlightEnabled(
        initialSettings.local.display.lineSelectorVisible);

    pendingLineNumber_ =
        initialSettings.control
            .instrument
            .lineNumber;

    lineNumberPersistTimer_ =
        new QTimer(this);

    lineNumberPersistTimer_->setSingleShot(
        true);

    lineNumberPersistTimer_->setInterval(
        300);

    connect(
        lineNumberPersistTimer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            const int lineNumber =
                pendingLineNumber_;

            settingsService_->update(
                [lineNumber](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .lineNumber =
                        lineNumber;
                });
        });

    const auto displayAspectRatio =
        initialSettings.local
            .display
            .aspectRatio;

    manualDisplayAspectRatio_ =
        displayAspectRatio;

    followWssAspectRatio_ =
        initialSettings.local
            .display
            .followWssAspectRatio;

    const auto& windowSettings =
        initialSettings.local.window;

    const double initialAspectRatio =
        OpenScopeSettings::aspectRatioValue(
            displayAspectRatio);

    const int initialWindowWidth =
        (std::max)(windowSettings.width, 640);

    const int initialWindowHeight =
        static_cast<int>(
            std::lround(
                static_cast<double>(initialWindowWidth) /
                initialAspectRatio));

    resize(
        initialWindowWidth,
        initialWindowHeight);

    move(
        windowSettings.x,
        windowSettings.y);

    workspace_ =
        new ScopeWorkspace(
            videoWidget_,
            waveformWidget_,
            vectorscopeWidget_,
            initialSettings,
            this);

    setCentralWidget(workspace_);

    philipsPatternRomSource_ =
        std::make_unique<PhilipsPatternRomSource>(
            videoEngine_);

    // Restore the last explicitly selected Blackmagic device before the
    // deferred startup probe runs.  deckLinkProbe() uses this preference when
    // its caller does not provide an explicit device index.
    {
        const QString settingsFileName =
            QDir(
                QCoreApplication::applicationDirPath())
                .filePath(
                    QStringLiteral(
                        "OpenScope.ini"));

        QSettings settings(
            settingsFileName,
            QSettings::IniFormat);

        selectedBlackmagicDeviceIndex_ =
            (std::max)(
                0,
                settings.value(
                    QStringLiteral(
                        "Local/Blackmagic/DeviceIndex"),
                    0)
                    .toInt());

        deckLinkSetPreferredDeviceIndex(
            selectedBlackmagicDeviceIndex_);
    }

    createSourceMenu();

    imageSourceTimer_ =
        new QTimer(this);
    imageSourceTimer_->setTimerType(Qt::PreciseTimer);
    imageSourceTimer_->setInterval(40);
    connect(
        imageSourceTimer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            publishImageFrame();
        });

    setAcceptDrops(true);

    // The saved width is the Qt client width.  Once the menu bar and central
    // workspace exist, normalize the initial height so the workspace itself
    // (not the decorated/client window) has the selected display aspect.
    QTimer::singleShot(
        0,
        this,
        [this]()
        {
            if (workspace_ == nullptr || f11FullScreen_ || customMaximized_)
            {
                return;
            }

            const int clientChromeWidth =
                (std::max)(0, width() - workspace_->width());

            const int clientChromeHeight =
                (std::max)(0, height() - workspace_->height());

            const int workspaceWidth =
                (std::max)(1, width() - clientChromeWidth);

            const int workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        windowAspectRatio()));

            resize(
                workspaceWidth + clientChromeWidth,
                workspaceHeight + clientChromeHeight);
        });

    connect(
        workspace_,
        &ScopeWorkspace::exportHighResolutionPngRequested,
        this,
        [this]()
        {
            const QString settingsFileName =
                QDir(
                    QCoreApplication::
                        applicationDirPath())
                    .filePath(
                        QStringLiteral(
                            "OpenScope.ini"));

            QSettings settings(
                settingsFileName,
                QSettings::IniFormat);

            QString exportDirectory =
                settings.value(
                    QStringLiteral(
                        "Local/Export/LastDirectory"))
                    .toString();

            if (exportDirectory.isEmpty() ||
                !QDir(exportDirectory).exists())
            {
                exportDirectory =
                    QStandardPaths::writableLocation(
                        QStandardPaths::PicturesLocation);
            }

            if (exportDirectory.isEmpty() ||
                !QDir(exportDirectory).exists())
            {
                exportDirectory =
                    QCoreApplication::
                        applicationDirPath();
            }

            QDir directory(
                exportDirectory);

            int sequenceNumber = 1;
            QString suggestedFileName;

            for (;;)
            {
                suggestedFileName =
                    QStringLiteral(
                        "capture_%1.png")
                        .arg(
                            sequenceNumber,
                            4,
                            10,
                            QLatin1Char('0'));

                if (!QFileInfo::exists(
                        directory.filePath(
                            suggestedFileName)))
                {
                    break;
                }

                ++sequenceNumber;
            }

            QString fileName =
                QFileDialog::getSaveFileName(
                    this,
                    tr("Export high-resolution PNG"),
                    directory.filePath(
                        suggestedFileName),
                    tr("PNG image (*.png)"));

            if (fileName.isEmpty())
            {
                return;
            }

            if (!fileName.endsWith(
                    QStringLiteral(".png"),
                    Qt::CaseInsensitive))
            {
                fileName +=
                    QStringLiteral(".png");
            }

            settings.setValue(
                QStringLiteral(
                    "Local/Export/LastDirectory"),
                QFileInfo(
                    fileName)
                    .absolutePath());

            const QImage image =
                videoEngine_->
                    captureHighResolutionSnapshot();

            if (image.isNull() ||
                !image.save(
                    fileName,
                    "PNG"))
            {
                QMessageBox::warning(
                    this,
                    tr("Export failed"),
                    tr("Could not capture or save the high-resolution PNG."));
            }
        });

    connect(
        workspace_,
        &ScopeWorkspace::exportHighResolutionPngQuickRequested,
        this,
        [this]()
        {
            const QString settingsFileName =
                QDir(
                    QCoreApplication::
                        applicationDirPath())
                    .filePath(
                        QStringLiteral(
                            "OpenScope.ini"));

            QSettings settings(
                settingsFileName,
                QSettings::IniFormat);

            QString exportDirectory =
                settings.value(
                    QStringLiteral(
                        "Local/Export/LastDirectory"))
                    .toString();

            if (exportDirectory.isEmpty() ||
                !QDir(exportDirectory).exists())
            {
                exportDirectory =
                    QStandardPaths::writableLocation(
                        QStandardPaths::PicturesLocation);
            }

            if (exportDirectory.isEmpty() ||
                !QDir(exportDirectory).exists())
            {
                exportDirectory =
                    QCoreApplication::
                        applicationDirPath();
            }

            QDir directory(
                exportDirectory);

            int sequenceNumber = 1;
            QString fileName;

            for (;;)
            {
                const QString suggestedFileName =
                    QStringLiteral(
                        "capture_%1.png")
                        .arg(
                            sequenceNumber,
                            4,
                            10,
                            QLatin1Char('0'));

                const QString candidatePath =
                    directory.filePath(
                        suggestedFileName);

                if (!QFileInfo::exists(
                        candidatePath))
                {
                    fileName = candidatePath;
                    break;
                }

                ++sequenceNumber;
            }

            settings.setValue(
                QStringLiteral(
                    "Local/Export/LastDirectory"),
                directory.absolutePath());

            const QImage image =
                videoEngine_->
                    captureHighResolutionSnapshot();

            if (image.isNull() ||
                !image.save(
                    fileName,
                    "PNG"))
            {
                QMessageBox::warning(
                    this,
                    tr("Export failed"),
                    tr("Could not capture or save the high-resolution PNG."));
            }
        });

    // Vectorscope follows the waveform screen-render contract.  QWidget
    // geometry is expressed in logical pixels, while the renderer should use
    // the physical pixel budget of a HiDPI display.  The completed image is
    // still presented into the same logical widget rectangle by Qt.
    const QSize initialVectorscopeLogicalSize =
        vectorscopeWidget_->renderSize();

    const QSize initialVectorscopeRenderSize =
        devicePixelRenderSize(
            vectorscopeWidget_,
            initialVectorscopeLogicalSize.width(),
            initialVectorscopeLogicalSize.height());

    videoEngine_->setVectorscopeOutputSize(
        initialVectorscopeRenderSize.width(),
        initialVectorscopeRenderSize.height());

    vectorscopeRenderSize_ =
        initialVectorscopeRenderSize;

    connect(
        vectorscopeWidget_,
        &VideoWidget::outputSizeChanged,
        this,
        [this](int width, int height)
        {
            const QSize renderSize =
                devicePixelRenderSize(
                    vectorscopeWidget_,
                    width,
                    height);

            videoEngine_->setVectorscopeOutputSize(
                renderSize.width(),
                renderSize.height());

            vectorscopeRenderSize_ =
                renderSize;

            if (activeRenderView_ ==
                RenderView::Vectorscope)
            {
                updateRenderResolutionTitle();
            }
        });

    connect(
        videoWidget_,
        &VideoWidget::outputSizeChanged,
        videoEngine_,
        &VideoEngine::setVideoOutputSize);

    connect(
        videoWidget_,
        &VideoWidget::outputSizeChanged,
        this,
        [this](int width, int height)
        {
            videoRenderSize_ =
                QSize(width, height);

            if (activeRenderView_ ==
                    RenderView::Video ||
                activeRenderView_ ==
                    RenderView::Matrix)
            {
                updateRenderResolutionTitle();
            }
        });

    connect(
        videoWidget_,
        &VideoWidget::leftInteractionStarted,
        this,
        [this]()
        {
            if (activeRenderView_ != RenderView::Matrix &&
                activeRenderView_ != RenderView::Video)
            {
                preVideoClickStateValid_ =
                    false;
                return;
            }

            const OpenScopeSettings& settings =
                settingsService_->settings();

            preVideoClickLineNumber_ =
                settings.control
                    .instrument
                    .lineNumber;

            preVideoClickScrollPosition_ =
                settings.control
                    .instrument
                    .waveform
                    .scrollPosition;

            preVideoClickStateValid_ =
                true;
        });

    connect(
        videoWidget_,
        &VideoWidget::doubleClickRestoreRequested,
        this,
        [this]()
        {
            if (!preVideoClickStateValid_ ||
                (activeRenderView_ != RenderView::Matrix &&
                 activeRenderView_ != RenderView::Video))
            {
                return;
            }

            workspace_->setLineNumber(
                preVideoClickLineNumber_);

            waveformWidget_->setScrollPosition(
                preVideoClickScrollPosition_);

            preVideoClickStateValid_ =
                false;
        });

    connect(
        videoWidget_,
        &VideoWidget::imageClicked,
        this,
        [this](double normalizedX,
               double normalizedY)
        {
            // Point-and-measure is valid both in matrix view and when the
            // video viewport is maximized. The selected line and X position
            // are instrument state, not a matrix-only UI action.
            if (activeRenderView_ != RenderView::Matrix &&
                activeRenderView_ != RenderView::Video)
            {
                return;
            }

            constexpr int kPalLineCount = 576;

            const int selectedLine =
                std::clamp(
                    static_cast<int>(
                        std::lround(
                            normalizedY *
                            static_cast<double>(
                                kPalLineCount - 1))),
                    0,
                    kPalLineCount - 1);

            // Updating the ControlWidget deliberately goes through its
            // existing valueChanged path. That keeps the spin box,
            // settings and VideoEngine selected-line state in sync.
            workspace_->setLineNumber(
                selectedLine);

            const int zoomFactor =
                waveformWidget_->zoomFactor();

            if (zoomFactor <= 1)
            {
                return;
            }

            // Centre the clicked source-X position in the visible
            // waveform window. The waveform renderer's scroll value is
            // 0..1 over the legal start-position range, not over the
            // complete source line.
            const double visibleFraction =
                1.0 /
                static_cast<double>(zoomFactor);

            const double maximumStart =
                1.0 -
                visibleFraction;

            const double requestedStart =
                normalizedX -
                (visibleFraction * 0.5);

            const double scrollPosition =
                maximumStart > 0.0
                ? std::clamp(
                    requestedStart /
                        maximumStart,
                    0.0,
                    1.0)
                : 0.0;

            waveformWidget_->setScrollPosition(
                scrollPosition);
        });

    const auto cycleWaveformZoom =
        [this]()
        {
            const int currentZoom =
                waveformWidget_->zoomFactor();

            const int nextZoom =
                currentZoom <= 1
                ? 5
                : (currentZoom <= 5
                    ? 10
                    : 1);

            workspace_->setWaveformZoomFactor(
                nextZoom);
        };

    const auto connectZoomCycle =
        [this, &cycleWaveformZoom](VideoWidget* widget)
        {
            connect(
                widget,
                &VideoWidget::rightClicked,
                this,
                [cycleWaveformZoom]()
                {
                    cycleWaveformZoom();
                });
        };

    connectZoomCycle(videoWidget_);
    connectZoomCycle(waveformWidget_);
    connectZoomCycle(vectorscopeWidget_);

    // Keyboard convenience controls for point-and-measure in the video view.
    // These deliberately reuse the existing line/zoom/scroll state paths.
    connect(
        videoWidget_,
        &VideoWidget::zoomInRequested,
        this,
        [this]()
        {
            if (activeRenderView_ != RenderView::Matrix &&
                activeRenderView_ != RenderView::Video)
            {
                return;
            }

            const int currentZoom = waveformWidget_->zoomFactor();
            const int nextZoom =
                currentZoom <= 1 ? 5 : 10;

            workspace_->setWaveformZoomFactor(nextZoom);
        });

    connect(
        videoWidget_,
        &VideoWidget::zoomOutRequested,
        this,
        [this]()
        {
            if (activeRenderView_ != RenderView::Matrix &&
                activeRenderView_ != RenderView::Video)
            {
                return;
            }

            const int currentZoom = waveformWidget_->zoomFactor();
            const int nextZoom =
                currentZoom >= 10 ? 5 : 1;

            workspace_->setWaveformZoomFactor(nextZoom);
        });

    // Video, waveform and vectorscope share one line/X navigation path.
    // This stays active in both matrix and maximized views.
    const auto stepInstrumentLine =
        [this](int delta)
        {
            constexpr int kAllLines = -1;
            constexpr int kFirstPalLine = 0;
            constexpr int kLastPalLine = 575;

            /*
             * Use the live navigation value, not the persisted settings
             * snapshot. Settings persistence is deliberately debounced,
             * so reading SettingsService here would make key/wheel repeat
             * advance only once per debounce interval.
             *
             * All Lines is a real navigation position at -1:
             *   Down from line 0 -> All Lines
             *   Up   from All Lines -> line 0
             */
            const int currentLine =
                std::clamp(
                    pendingLineNumber_,
                    kAllLines,
                    kLastPalLine);

            const int nextLine =
                std::clamp(
                    currentLine + delta,
                    kAllLines,
                    kLastPalLine);

            workspace_->setLineNumber(nextLine);
        };

    const auto panInstrumentWaveform =
        [this](double delta)
        {
            if (waveformWidget_->zoomFactor() <= 1)
            {
                return;
            }

            constexpr double kKeyboardPanStep = 0.00625;

            const double currentPosition =
                settingsService_->settings()
                    .control
                    .instrument
                    .waveform
                    .scrollPosition;

            waveformWidget_->setScrollPosition(
                std::clamp(
                    currentPosition +
                        delta * kKeyboardPanStep,
                    0.0,
                    1.0));
        };

    const auto connectInstrumentArrows =
        [this,
         &stepInstrumentLine,
         &panInstrumentWaveform](VideoWidget* widget)
        {
            connect(
                widget,
                &VideoWidget::lineUpRequested,
                this,
                [stepInstrumentLine]()
                {
                    stepInstrumentLine(-1);
                });

            connect(
                widget,
                &VideoWidget::lineDownRequested,
                this,
                [stepInstrumentLine]()
                {
                    stepInstrumentLine(+1);
                });

            connect(
                widget,
                &VideoWidget::panLeftRequested,
                this,
                [panInstrumentWaveform]()
                {
                    panInstrumentWaveform(-1.0);
                });

            connect(
                widget,
                &VideoWidget::panRightRequested,
                this,
                [panInstrumentWaveform]()
                {
                    panInstrumentWaveform(+1.0);
                });
        };

    connectInstrumentArrows(videoWidget_);
    connectInstrumentArrows(waveformWidget_);
    connectInstrumentArrows(vectorscopeWidget_);

    connect(
        videoWidget_,
        &VideoWidget::multiburstRequested,
        this,
        [this]()
        {
            // M behaves identically whether keyboard focus is on the
            // waveform or on the video view. Keep the implementation in
            // WaveformWidget so there is only one multiburst code path.
            waveformWidget_->triggerMultiburstMeasurement();
        });

    const auto showYSpectrum =
        [this]()
        {
            ySpectrumWindow_->show();
            ySpectrumWindow_->raise();
            ySpectrumWindow_->activateWindow();
        };

    connect(
        videoWidget_,
        &VideoWidget::spectrumRequested,
        this,
        showYSpectrum);

    connect(
        waveformWidget_,
        &WaveformWidget::probePresentationChanged,
        this,
        [this](
            bool active,
            double normalizedX,
            double volts)
        {
            videoEngine_->setWaveformMeasurementProbePresentation(
                active,
                normalizedX,
                volts);
        });

    connect(
        waveformWidget_,
        &WaveformWidget::outputSizeChanged,
        this,
        [this](int width, int height)
        {
            // Render the screen waveform at the widget's physical-pixel
            // resolution.  WaveformWidget::paintEvent() tags that QImage with
            // the matching DPR before presentation, so Qt maps physical image
            // pixels to physical screen pixels instead of resampling the image
            // as though it were a logical-pixel bitmap.
            const QSize renderSize =
                devicePixelRenderSize(
                    waveformWidget_,
                    width,
                    height);

            videoEngine_->setWaveformOutputSize(
                renderSize.width(),
                renderSize.height());

            waveformRenderSize_ =
                renderSize;

            if (activeRenderView_ ==
                RenderView::Waveform)
            {
                updateRenderResolutionTitle();
            }
        });

    connect(
        videoEngine_,
        &VideoEngine::frameChanged,
        videoWidget_,
        &VideoWidget::setImage);

    connect(
        videoEngine_,
        &VideoEngine::frameChanged,
        this,
        [this](const QImage&)
        {
            ++videoOpenScopeFrameCount_;
        });

    auto* inputSignalWatchdog =
        new QTimer(this);

    inputSignalWatchdog->setSingleShot(true);
    inputSignalWatchdog->setInterval(500);

    const auto setInputSignalValid =
        [this](bool valid)
        {
            videoWidget_->setInputSignalValid(valid);
            waveformWidget_->setInputSignalValid(valid);
            vectorscopeWidget_->setInputSignalValid(valid);
        };

    connect(
        videoEngine_,
        &VideoEngine::inputSignalStateChanged,
        this,
        [inputSignalWatchdog, setInputSignalValid](bool valid)
        {
            setInputSignalValid(valid);
            inputSignalWatchdog->start();
        });

    connect(
        inputSignalWatchdog,
        &QTimer::timeout,
        this,
        [setInputSignalValid]()
        {
            setInputSignalValid(false);
        });

    // No source has delivered a frame yet. Start in the honest state.
    setInputSignalValid(false);

    auto* videoSpoutOutput =
        new SpoutOutput(
            QStringLiteral("OpenScope Video"),
            this);

    connect(
        videoSpoutOutput,
        &SpoutOutput::submissionTiming,
        this,
        [this](std::uint64_t, std::uint64_t, std::uint64_t)
        {
            ++videoSpoutFrameCount_;
        });

    connect(
        videoEngine_,
        &VideoEngine::videoSpoutChanged,
        videoSpoutOutput,
        &SpoutOutput::submitImage,
        Qt::QueuedConnection);

    connect(
        videoEngine_,
        &VideoEngine::inputSignalStateChanged,
        videoSpoutOutput,
        &SpoutOutput::setInputSignalValid,
        Qt::QueuedConnection);

    const auto setVideoSpoutEnabled =
        [this, videoSpoutOutput](bool enabled)
        {
            // Gate the sender itself as well as the renderer. This prevents
            // an already queued frame from reopening Spout after OFF.
            videoSpoutOutput->setEnabled(
                enabled);

            videoEngine_->setSpoutVideoEnabled(
                enabled);
        };

    connect(
        workspace_,
        &ScopeWorkspace::spoutVideoEnabledChanged,
        this,
        [this, setVideoSpoutEnabled](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local
                        .spout
                        .videoEnabled =
                        enabled;
                });

            setVideoSpoutEnabled(
                enabled);
        });

    setVideoSpoutEnabled(
        initialSettings.local
            .spout
            .videoEnabled);

    connect(
        videoEngine_,
        &VideoEngine::waveformChanged,
        waveformWidget_,
        &WaveformWidget::setImage);

    connect(
        videoEngine_,
        &VideoEngine::waveformChanged,
        this,
        [this](const QImage&)
        {
            ++waveformOpenScopeFrameCount_;
        });

    connect(
        videoEngine_,
        &VideoEngine::waveformMeasurementDataChanged,
        waveformWidget_,
        &WaveformWidget::setMeasurementLuma);

    connect(
        videoEngine_,
        &VideoEngine::waveformSpectrumDataChanged,
        this,
        [this](
            const QVector<float>& fullLine,
            const QVector<float>& visiblePart,
            bool inputSignalValid)
        {
            const auto& settings =
                settingsService_->settings();

            waveformWidget_->setSnrSamples(
                fullLine,
                inputSignalValid);

            ySpectrumWindow_->setSamples(
                fullLine,
                visiblePart,
                settings.control.instrument.lineNumber,
                settings.control.instrument.waveform.zoom,
                inputSignalValid);
        });

    connect(
        ySpectrumWindow_,
        &YSpectrumWindow::flatFieldCaptureRequested,
        videoEngine_,
        &VideoEngine::requestWaveformFlatFieldSpectrum);

    connect(
        videoEngine_,
        &VideoEngine::waveformFlatFieldSpectrumDataChanged,
        ySpectrumWindow_,
        &YSpectrumWindow::setFlatFieldSamples);

    auto* waveformVideoPreview =
        new WaveformVideoPreview(this);

    connect(
        videoEngine_,
        &VideoEngine::waveformVideoChanged,
        waveformVideoPreview,
        [waveformVideoPreview](const QImage& image)
        {
            waveformVideoPreview->setImage(image);
        });

    // Spout is opt-in. When disabled there is no waveform -> Spout
    // connection at all, so no image upload or sender work is performed.
    auto* waveformSpoutOutput =
        new SpoutOutput(
            QStringLiteral("OpenScope Waveform"),
            this);

    connect(
        waveformSpoutOutput,
        &SpoutOutput::submissionTiming,
        this,
        [this](std::uint64_t, std::uint64_t, std::uint64_t)
        {
            ++waveformSpoutFrameCount_;
        });

    connect(
        videoEngine_,
        &VideoEngine::inputSignalStateChanged,
        waveformSpoutOutput,
        &SpoutOutput::setInputSignalValid,
        Qt::QueuedConnection);

    auto waveformSpoutConnection =
        std::make_shared<QMetaObject::Connection>();

    const auto setWaveformSpoutEnabled =
        [this,
         waveformSpoutOutput,
         waveformSpoutConnection](bool enabled)
        {
            QObject::disconnect(
                *waveformSpoutConnection);

            *waveformSpoutConnection =
                QMetaObject::Connection();

            waveformSpoutOutput->setEnabled(
                enabled);

            // The waveform PAL/video renderer is demand driven. Without
            // this call waveformVideoEnabled_ remains false and no
            // waveformVideoChanged frames are ever produced.
            videoEngine_->setWaveformVideoEnabled(
                enabled);

            if (enabled)
            {
                *waveformSpoutConnection =
                    connect(
                        videoEngine_,
                        &VideoEngine::waveformVideoChanged,
                        waveformSpoutOutput,
                        &SpoutOutput::submitImage,
                        Qt::QueuedConnection);
            }
        };

    connect(
        workspace_,
        &ScopeWorkspace::spoutWaveformEnabledChanged,
        this,
        [this, setWaveformSpoutEnabled](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local
                        .spout
                        .waveformEnabled =
                        enabled;
                });

            setWaveformSpoutEnabled(
                enabled);
        });

    setWaveformSpoutEnabled(
        initialSettings.local
            .spout
            .waveformEnabled);

    waveformVideoPreview->setVisible(
        initialSettings.local
            .floaties
            .waveformVideoVisible);

    connect(
        videoEngine_,
        &VideoEngine::vectorscopeChanged,
        vectorscopeWidget_,
        &VideoWidget::setImage);

    connect(
        videoEngine_,
        &VideoEngine::vectorscopeChanged,
        this,
        [this](const QImage&)
        {
            ++vectorscopeOpenScopeFrameCount_;
        });

    VectorscopePresentationInfo vectorscopePresentation;
    vectorscopePresentation.source = blackmagicDeviceName_;
    vectorscopePresentation.input = QStringLiteral("Composite");
    vectorscopePresentation.standard = QStringLiteral("625/50d");
    vectorscopePresentation.targets =
        initialSettings.control.instrument.vectorscope.showHundredPercentTargets
        ? QStringLiteral("100%")
        : QStringLiteral("75%");
    vectorscopePresentation.matrix = QStringLiteral("BT.601");
    vectorscopePresentation.processing = QStringLiteral("YUV 4:2:2 10 bit");

    videoEngine_->setVectorscopePresentationInfo(
        vectorscopePresentation);

    auto* vectorscopeSpoutOutput =
        new SpoutOutput(
            QStringLiteral("OpenScope Vectorscope"),
            this);

    connect(
        vectorscopeSpoutOutput,
        &SpoutOutput::submissionTiming,
        this,
        [this](std::uint64_t, std::uint64_t, std::uint64_t)
        {
            ++vectorscopeSpoutFrameCount_;
        });

    connect(
        videoEngine_,
        &VideoEngine::inputSignalStateChanged,
        vectorscopeSpoutOutput,
        &SpoutOutput::setInputSignalValid,
        Qt::QueuedConnection);

    auto vectorscopeSpoutConnection =
        std::make_shared<QMetaObject::Connection>();

    const auto setVectorscopeSpoutEnabled =
        [this,
         vectorscopeSpoutOutput,
         vectorscopeSpoutConnection](bool enabled)
        {
            QObject::disconnect(
                *vectorscopeSpoutConnection);

            *vectorscopeSpoutConnection =
                QMetaObject::Connection();

            constexpr VideoStandard videoStandard =
                VideoStandard::pal625();

            videoEngine_->setVectorscopeVideoOutputSize(
                videoStandard.outputWidth,
                videoStandard.outputHeight);

            videoEngine_->setVectorscopeVideoContentScale(
                videoStandard.safeWidthScale,
                videoStandard.safeHeightScale);

            vectorscopeSpoutOutput->setEnabled(
                enabled);

            videoEngine_->setVectorscopeVideoEnabled(
                enabled);

            if (enabled)
            {
                *vectorscopeSpoutConnection =
                    connect(
                        videoEngine_,
                        &VideoEngine::vectorscopeVideoChanged,
                        vectorscopeSpoutOutput,
                        &SpoutOutput::submitImage,
                        Qt::QueuedConnection);
            }
        };

    connect(
        workspace_,
        &ScopeWorkspace::spoutVectorscopeEnabledChanged,
        this,
        [this, setVectorscopeSpoutEnabled](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.spout.vectorscopeEnabled = enabled;
                });

            setVectorscopeSpoutEnabled(enabled);
        });

    setVectorscopeSpoutEnabled(
        initialSettings.local.spout.vectorscopeEnabled);

    viewFpsTimer_ =
        new QTimer(this);
    viewFpsTimer_->setInterval(1000);

    connect(
        viewFpsTimer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            const qint64 elapsedMs =
                viewFpsElapsedTimer_.isValid()
                ? viewFpsElapsedTimer_.restart()
                : 1000;

            const double scale =
                elapsedMs > 0
                ? 1000.0 / static_cast<double>(elapsedMs)
                : 1.0;

            if (workspace_ != nullptr)
            {
                workspace_->setViewFps(
                    static_cast<double>(videoOpenScopeFrameCount_) * scale,
                    static_cast<double>(videoSpoutFrameCount_) * scale,
                    static_cast<double>(waveformOpenScopeFrameCount_) * scale,
                    static_cast<double>(waveformSpoutFrameCount_) * scale,
                    static_cast<double>(vectorscopeOpenScopeFrameCount_) * scale,
                    static_cast<double>(vectorscopeSpoutFrameCount_) * scale);
            }

            videoOpenScopeFrameCount_ = 0;
            videoSpoutFrameCount_ = 0;
            waveformOpenScopeFrameCount_ = 0;
            waveformSpoutFrameCount_ = 0;
            vectorscopeOpenScopeFrameCount_ = 0;
            vectorscopeSpoutFrameCount_ = 0;
        });

    viewFpsElapsedTimer_.start();
    viewFpsTimer_->start();

    // Initial processing/instrument state.
    videoEngine_->setDisplayGamma(
        initialSettings.local
            .display
            .gamma);

    videoEngine_->setNoiseReductionEnabled(
        initialSettings.control
            .processing
            .noiseFilter
            .enabled);

    videoEngine_->setNoiseReductionIntensity(
        initialSettings.control
            .processing
            .noiseFilter
            .strength);

    videoEngine_->setPcmDecoderEnabled(
        initialSettings.control
            .pcmAudio
            .enabled);

    videoEngine_->setPcmAudioOutputBackend(
        initialSettings.control.pcmAudio.outputBackend);

    videoEngine_->setPcmAudioOutputDevice(
        initialSettings.control.pcmAudio.outputDeviceId);

    // Restore the saved ASIO driver before enabling output. AsioPcmOutput does
    // not open it until decoded PCM establishes the source sample rate, so this
    // uses the same restart path as an explicit UI driver selection/toggle.
    videoEngine_->setPcmAsioDriver(
        initialSettings.control.pcmAudio.asioDriverName);

    videoEngine_->setPcmAudioBufferMs(
        initialSettings.control.pcmAudio.outputBufferMs);

    videoEngine_->setPcmAudioOutputEnabled(
        initialSettings.control.pcmAudio.outputEnabled);

    // Bottom-two-bit muting was an analysis aid and is intentionally retired.
    videoEngine_->setPcmMuteBottomTwoBits(false);

    videoEngine_->setPcmDeEmphasisMode(
        initialSettings.control.pcmAudio.deEmphasisMode);

    videoEngine_->setLumaCompensationEnabled(
        blackmagicSourceActive_ &&
        initialSettings.control
            .processing
            .lumaCompensation
            .enabled);

    videoEngine_->setLumaCompensationGainHundredthsDb(
        initialSettings.control
            .processing
            .lumaCompensation
            .gainHundredthsDb);

    videoEngine_->setWaveformColor(
        !initialSettings.control
            .instrument
            .waveform
            .vintageLook);

    videoEngine_->setWaveformAntiAliasing(
        initialSettings.control
            .instrument
            .waveform
            .antiAliasing);

    videoEngine_->setWaveformColorizeIllegalLuminance(
        initialSettings.control
            .instrument
            .waveform
            .colorizeIllegalLuminance);

    videoEngine_->setVectorscopeColorizeGamutErrors(
        initialSettings.control
            .instrument
            .vectorscope
            .colorizeGamutErrors);

    videoEngine_->setWaveformChromaFillIntensity(
        initialSettings.control
            .instrument
            .waveform
            .chromaRenderIntensity);

    videoEngine_->setSelectedLine(
        initialSettings.control
            .instrument
            .lineNumber);

    waveformWidget_->setSelectedLine(
        initialSettings.control
            .instrument
            .lineNumber);

    videoEngine_->setWaveformPersistence(
        initialSettings.control
            .instrument
            .waveform
            .persistenceFrames);

    videoEngine_->setWaveformCoreIntensity(
        initialSettings.control
            .instrument
            .waveform
            .coreIntensity);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        videoEngine_->setWaveformCoreWidth(
            initialSettings.control
                .instrument
                .waveform
                .coreWidthTenths);
    }


    videoEngine_->setWaveformVideoContentScale(
        initialSettings.control
            .videoOut
            .underscan);

    videoEngine_->setWaveformVideoAspectRatio(
        initialSettings.control
            .videoOut
            .aspectRatio);

    videoEngine_->setVectorscopeGlow(
        initialSettings.control
            .instrument
            .vectorscope
            .glow);

    videoEngine_->setWaveformScrollPosition(
        initialSettings.control
            .instrument
            .waveform
            .scrollPosition);

    waveformWidget_->setScrollPosition(
        initialSettings.control
            .instrument
            .waveform
            .scrollPosition);

    waveformWidget_->setZoomEnabled(
        initialSettings.control
            .instrument
            .lineNumber >= 0);

    waveformWidget_->setZoomFactor(
        initialSettings.control
            .instrument
            .waveform
            .zoom);

    videoEngine_->setWaveformZoomFactor(
        initialSettings.control
            .instrument
            .waveform
            .zoom);

    applyDisplayAspectRatio(
        displayAspectRatio,
        false);

    // Power policy is opt-in. Default is false; when enabled, keep the
    // display/system idle timers alive only for this OpenScope process.
    const auto applyPreventDisplaySleep =
        [this](bool enabled)
        {
            if (enabled)
            {
                const EXECUTION_STATE result =
                    SetThreadExecutionState(
                        ES_CONTINUOUS |
                        ES_DISPLAY_REQUIRED |
                        ES_SYSTEM_REQUIRED);

                preventDisplaySleepActive_ =
                    result != 0;
            }
            else
            {
                SetThreadExecutionState(ES_CONTINUOUS);
                preventDisplaySleepActive_ = false;
            }
        };

    applyPreventDisplaySleep(
        initialSettings.local.display.preventDisplaySleep);

    connect(
        workspace_,
        &ScopeWorkspace::preventDisplaySleepChanged,
        this,
        [this, applyPreventDisplaySleep](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.display.preventDisplaySleep = enabled;
                });

            applyPreventDisplaySleep(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::followWssAspectRatioChanged,
        this,
        [this](bool enabled)
        {
            followWssAspectRatio_ =
                enabled;

            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.display.followWssAspectRatio = enabled;
                });

            if (!enabled)
            {
                applyDisplayAspectRatio(
                    manualDisplayAspectRatio_,
                    true);

                return;
            }

            if (wssLocked_ &&
                wssRecommendedDisplayAspect_ >= 0)
            {
                const auto aspectRatio =
                    wssRecommendedDisplayAspect_ == 0
                    ? OpenScopeSettings::AspectRatio::Ratio4x3
                    : OpenScopeSettings::AspectRatio::Ratio16x9;

                applyDisplayAspectRatio(
                    aspectRatio,
                    true);
            }
        });

    connect(
        workspace_,
        &ScopeWorkspace::injectTestWss4x3Requested,
        videoEngine_,
        &VideoEngine::injectTestWss4x3);

    connect(
        workspace_,
        &ScopeWorkspace::injectTestWss16x9Requested,
        videoEngine_,
        &VideoEngine::injectTestWss16x9);

    connect(
        videoEngine_,
        &VideoEngine::pcmAudioLevelsChanged,
        this,
        [this](float leftPeak, float rightPeak)
        {
            if (workspace_ != nullptr)
                workspace_->setPcmAudioLevels(leftPeak, rightPeak);
        });

    connect(
        videoEngine_,
        &VideoEngine::pcmAudioScopeSamplesChanged,
        this,
        [this](const QByteArray& stereoPcm16)
        {
            if (workspace_ != nullptr)
                workspace_->setPcmAudioScopeSamples(stereoPcm16);
        });

    connect(
        videoEngine_,
        &VideoEngine::pcmDecoderStatusChanged,
        this,
        [this](
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
            if (workspace_ != nullptr)
            {
                workspace_->setPcmDecoderStatus(
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
        });

    connect(
        videoEngine_,
        &VideoEngine::wssStateChanged,
        this,
        [this](
            const QString& status,
            bool locked,
            int recommendedDisplayAspect)
        {
            workspace_->setWssStatus(
                status);

            const bool wasLocked =
                wssLocked_;

            wssLocked_ =
                locked;

            wssRecommendedDisplayAspect_ =
                recommendedDisplayAspect;

            if (!followWssAspectRatio_)
            {
                return;
            }

            if (locked &&
                recommendedDisplayAspect >= 0)
            {
                const auto aspectRatio =
                    recommendedDisplayAspect == 0
                    ? OpenScopeSettings::AspectRatio::Ratio4x3
                    : OpenScopeSettings::AspectRatio::Ratio16x9;

                applyDisplayAspectRatio(
                    aspectRatio,
                    true);
            }
            else if (wasLocked &&
                     !locked)
            {
                applyDisplayAspectRatio(
                    manualDisplayAspectRatio_,
                    true);
            }
        });

    // Control-panel wiring.
    connect(
        workspace_,
        &ScopeWorkspace::lineNumberChanged,
        this,
        [this](int lineNumber)
        {
            // Navigation must stay cheap: publish the latest selected line
            // immediately and coalesce persistent settings writes.
            pendingLineNumber_ =
                lineNumber;

            if (lineNumberPersistTimer_ != nullptr)
            {
                lineNumberPersistTimer_->start();
            }

            videoEngine_->setSelectedLine(
                lineNumber);

            waveformWidget_->setSelectedLine(
                lineNumber);

            waveformWidget_->setZoomEnabled(
                lineNumber >= 0);
        });

    connect(
        workspace_,
        &ScopeWorkspace::waveformZoomChanged,
        this,
        [this](int zoomFactor)
        {
            settingsService_->update(
                [zoomFactor](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .zoom =
                        zoomFactor;
                });

            waveformWidget_->setZoomFactor(
                zoomFactor);

            videoEngine_->setWaveformZoomFactor(
                zoomFactor);
        });

    connect(
        waveformWidget_,
        &WaveformWidget::scrollPositionChanged,
        this,
        [this](double position)
        {
            settingsService_->update(
                [position](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .scrollPosition =
                        position;
                });

            videoEngine_->setWaveformScrollPosition(
                position);
        });

    connect(
        workspace_,
        &ScopeWorkspace::waveformPersistenceChanged,
        this,
        [this](int persistence)
        {
            settingsService_->update(
                [persistence](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .persistenceFrames =
                        persistence;
                });

            videoEngine_->setWaveformPersistence(
                persistence);
        });

    connect(
        workspace_,
        &ScopeWorkspace::waveformCoreIntensityChanged,
        this,
        [this](int intensity)
        {
            const int clampedIntensity =
                std::clamp(
                    intensity,
                    0,
                    100);

            settingsService_->update(
                [clampedIntensity](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .coreIntensity =
                        clampedIntensity;
                });

            videoEngine_->setWaveformCoreIntensity(
                clampedIntensity);
        });

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        connect(
            workspace_,
            &ScopeWorkspace::waveformCoreWidthChanged,
            this,
            [this](int widthTenths)
            {
                const int clampedWidthTenths =
                    std::clamp(
                        widthTenths,
                        5,
                        30);

                settingsService_->update(
                    [clampedWidthTenths](OpenScopeSettings& settings)
                    {
                        settings.control
                            .instrument
                            .waveform
                            .coreWidthTenths =
                            clampedWidthTenths;
                    });

                videoEngine_->setWaveformCoreWidth(
                    clampedWidthTenths);
            });
    }

    connect(
        workspace_,
        &ScopeWorkspace::vectorscopeGlowChanged,
        this,
        [this](int glow)
        {
            settingsService_->update(
                [glow](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .vectorscope
                        .glow =
                        glow;
                });

            videoEngine_->setVectorscopeGlow(
                glow);
        });

    connect(
        workspace_,
        &ScopeWorkspace::waveformChromaFillIntensityChanged,
        this,
        [this](int intensity)
        {
            settingsService_->update(
                [intensity](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .chromaRenderIntensity =
                        intensity;
                });

            videoEngine_->setWaveformChromaFillIntensity(
                intensity);
        });

    connect(
        workspace_,
        &ScopeWorkspace::waveformColorChanged,
        this,
        [this](bool colorEnabled)
        {
            settingsService_->update(
                [colorEnabled](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .vintageLook =
                        !colorEnabled;
                });

            videoEngine_->setWaveformColor(
                colorEnabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::antiAliasingChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control
                        .instrument
                        .waveform
                        .antiAliasing = enabled;
                });

            videoEngine_->setWaveformAntiAliasing(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::colorizeIllegalLuminanceChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.instrument.waveform.colorizeIllegalLuminance = enabled;
                });

            videoEngine_->setWaveformColorizeIllegalLuminance(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::colorizeGamutErrorsChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.instrument.vectorscope.colorizeGamutErrors = enabled;
                });

            videoEngine_->setVectorscopeColorizeGamutErrors(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::lineSelectorVisibleChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.display.lineSelectorVisible = enabled;
                });

            videoEngine_->setVideoLineHighlightEnabled(
                enabled);
        });

    const auto updateSafetyAreas =
        [this]()
        {
            const auto& display =
                settingsService_->settings().local.display;
            videoWidget_->setSafetyAreas(
                display.safetyArea90,
                display.textSafetyArea80);
        };

    connect(
        workspace_,
        &ScopeWorkspace::safetyArea90Changed,
        this,
        [this, updateSafetyAreas](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.display.safetyArea90 = enabled;
                });
            updateSafetyAreas();
        });

    connect(
        workspace_,
        &ScopeWorkspace::textSafetyArea80Changed,
        this,
        [this, updateSafetyAreas](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.local.display.textSafetyArea80 = enabled;
                });
            updateSafetyAreas();
        });

    connect(
        workspace_,
        &ScopeWorkspace::noiseReductionChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control
                        .processing
                        .noiseFilter
                        .enabled =
                        enabled;
                });

            videoEngine_->setNoiseReductionEnabled(
                enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::noiseReductionIntensityChanged,
        this,
        [this](int strength)
        {
            settingsService_->update(
                [strength](OpenScopeSettings& settings)
                {
                    settings.control
                        .processing
                        .noiseFilter
                        .strength =
                        strength;
                });

            videoEngine_->setNoiseReductionIntensity(
                strength);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioScopeRequestedChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.audioScopeEnabled = enabled;
                });
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioScopeColorizedChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.audioScopeColorized = enabled;
                });
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioScopeActiveChanged,
        this,
        [this](bool active)
        {
            pcmAudioScopeActive_ = active;
            videoEngine_->setVectorscopeSuppressedForAudioScope(active);
            updateScreenRenderDemand();
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmDecoderEnabledChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.enabled = enabled;
                });

            videoEngine_->setPcmDecoderEnabled(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmDecoderModeChanged,
        this,
        [this](int mode)
        {
            videoEngine_->setPcmDecoderMode(mode);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioOutputEnabledChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.outputEnabled = enabled;
                });
            videoEngine_->setPcmAudioOutputEnabled(enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioOutputBackendChanged,
        this,
        [this](int backend)
        {
            settingsService_->update([backend](OpenScopeSettings& settings)
            {
                settings.control.pcmAudio.outputBackend = backend;
            });
            videoEngine_->setPcmAudioOutputBackend(backend);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAsioDriverChanged,
        this,
        [this](const QString& driverName)
        {
            const std::string name = driverName.toStdString();
            settingsService_->update([name](OpenScopeSettings& settings)
            {
                settings.control.pcmAudio.asioDriverName = name;
            });
            videoEngine_->setPcmAsioDriver(name);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAsioBufferLoggingChanged,
        this,
        [this](bool enabled)
        {
            videoEngine_->setPcmAsioBufferLoggingEnabled(enabled);
        });


    connect(
        workspace_,
        &ScopeWorkspace::pcmAsioTargetMsChanged,
        this,
        [this](double milliseconds)
        {
            videoEngine_->setPcmAsioTargetMs(milliseconds);
        });

    // Live ASIO diagnostics for the PCM Details floaty / buffer graph.
    auto* pcmAsioStatusTimer = new QTimer(this);
    pcmAsioStatusTimer->setInterval(100);
    connect(
        pcmAsioStatusTimer,
        &QTimer::timeout,
        this,
        [this, lastCb = std::uint64_t{0}]() mutable
        {
            if (workspace_ == nullptr || videoEngine_ == nullptr)
                return;
            const auto cb = videoEngine_->pcmAsioCallbackCount();
            const double cbRate = cb >= lastCb ? static_cast<double>(cb - lastCb) * 10.0 : 0.0;
            lastCb = cb;
            workspace_->setPcmAsioStatus(
                videoEngine_->pcmAsioSampleRate(),
                videoEngine_->pcmAsioBufferFrames(),
                videoEngine_->pcmAsioBufferPeriodMs(),
                videoEngine_->pcmAsioTargetMs(),
                videoEngine_->pcmAsioLowWaterMs(),
                videoEngine_->pcmAsioFastFillMs(),
                videoEngine_->pcmAsioAverage3sFillMs(),
                videoEngine_->pcmAsioNudgePpm(),
                cbRate,
                videoEngine_->pcmAsioUnderruns(),
                videoEngine_->pcmAsioOverruns());
        });
    pcmAsioStatusTimer->start();

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioOutputDeviceChanged,
        this,
        [this](const QString& deviceId)
        {
            const std::string id =
                deviceId.toStdString();

            settingsService_->update(
                [id](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.outputDeviceId =
                        id;
                });

            videoEngine_->setPcmAudioOutputDevice(
                id);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmAudioBufferMsChanged,
        this,
        [this](int milliseconds)
        {
            settingsService_->update(
                [milliseconds](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.outputBufferMs = milliseconds;
                });
            videoEngine_->setPcmAudioBufferMs(milliseconds);
        });

    connect(
        workspace_,
        &ScopeWorkspace::pcmDeEmphasisModeChanged,
        this,
        [this](int mode)
        {
            const int clamped = std::clamp(mode, 0, 2);
            settingsService_->update(
                [clamped](OpenScopeSettings& settings)
                {
                    settings.control.pcmAudio.deEmphasisMode = clamped;
                });
            videoEngine_->setPcmDeEmphasisMode(clamped);
        });

    connect(
        workspace_,
        &ScopeWorkspace::lumaCompensationChanged,
        this,
        [this](bool enabled)
        {
            settingsService_->update(
                [enabled](OpenScopeSettings& settings)
                {
                    settings.control
                        .processing
                        .lumaCompensation
                        .enabled =
                        enabled;
                });

            videoEngine_->setLumaCompensationEnabled(
                blackmagicSourceActive_ && enabled);
        });

    connect(
        workspace_,
        &ScopeWorkspace::lumaCompensationGainChanged,
        this,
        [this](int gainHundredthsDb)
        {
            settingsService_->update(
                [gainHundredthsDb](OpenScopeSettings& settings)
                {
                    settings.control
                        .processing
                        .lumaCompensation
                        .gainHundredthsDb =
                        gainHundredthsDb;
                });

            videoEngine_->setLumaCompensationGainHundredthsDb(
                gainHundredthsDb);
        });

    connect(
        workspace_,
        &ScopeWorkspace::compositeLumaGainChanged,
        this,
        [](int gainHundredthsDb)
        {
            deckLinkSetCompositeLumaGain(
                static_cast<double>(gainHundredthsDb) / 100.0);
        });

    connect(
        workspace_,
        &ScopeWorkspace::compositeChromaGainChanged,
        this,
        [](int gainHundredthsDb)
        {
            deckLinkSetCompositeChromaGain(
                static_cast<double>(gainHundredthsDb) / 100.0);
        });

    connect(
        workspace_,
        &ScopeWorkspace::compositeGainCommitRequested,
        this,
        []()
        {
            deckLinkCommitConfiguration();
        });

    connect(
        workspace_,
        &ScopeWorkspace::legacyAspectRatioChanged,
        this,
        [this](bool legacyEnabled)
        {
            const auto aspectRatio =
                legacyEnabled
                ? OpenScopeSettings::AspectRatio::Ratio4x3
                : OpenScopeSettings::AspectRatio::Ratio16x9;

            manualDisplayAspectRatio_ =
                aspectRatio;

            settingsService_->update(
                [aspectRatio](OpenScopeSettings& settings)
                {
                    settings.local
                        .display
                        .aspectRatio =
                        aspectRatio;
                });

            if (!followWssAspectRatio_ ||
                !wssLocked_)
            {
                applyDisplayAspectRatio(
                    aspectRatio,
                    true);
            }
        });

    performanceWidget_ =
        new PerformanceWidget(this);

    performanceWidget_->setWindowTitle(
        "OpenScope Performance");

    performanceWidget_->setWindowFlag(
        Qt::Tool);

    performanceWidget_->resize(
        performanceWidget_->sizeHint());

    if (initialSettings.local
        .floaties
        .performance
        .positionValid)
    {
        performanceWidget_->move(
            initialSettings.local
                .floaties
                .performance
                .x,
            initialSettings.local
                .floaties
                .performance
                .y);
    }

    connect(
        performanceWidget_,
        &PerformanceWidget::visibilityChanged,
        this,
        [this](bool visible)
        {
            workspace_->setPerformanceVisible(
                visible);

            settingsService_->update(
                [visible](OpenScopeSettings& settings)
                {
                    settings.local
                        .floaties
                        .performanceVisible =
                        visible;
                });
        });

    connect(
        workspace_,
        &ScopeWorkspace::performanceVisibilityChanged,
        this,
        [this](bool visible)
        {
            settingsService_->update(
                [visible](OpenScopeSettings& settings)
                {
                    settings.local
                        .floaties
                        .performanceVisible =
                        visible;
                });

            performanceWidget_->setVisible(
                visible);
        });

    performanceWidget_->setVisible(
        initialSettings.local
            .floaties
            .performanceVisible);

    connect(
        workspace_,
        &ScopeWorkspace::floatiesHomeRequested,
        this,
        &MainWindow::homeFloaties);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        connect(
            workspace_,
            &ScopeWorkspace::waveformRawCaptureRequested,
            this,
            [this]()
            {
                const QString fileName =
                    QFileDialog::getSaveFileName(
                        this,
                        QStringLiteral(
                            "Capture waveform RAW"),
                        QDir(
                            QCoreApplication::applicationDirPath())
                            .filePath(
                                QStringLiteral(
                                    "waveform_capture.raw")),
                        QStringLiteral(
                            "RAW waveform (*.raw)"));

                if (fileName.isEmpty())
                {
                    return;
                }

                if (!videoEngine_->startWaveformRawCapture(
                        fileName.toStdString()))
                {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Waveform RAW capture"),
                        QStringLiteral(
                            "Select one waveform line first (All Lines cannot be captured), and make sure no capture is already running."));
                }
            });
    }

    connect(
        workspace_,
        &ScopeWorkspace::workspaceViewChanged,
        this,
        [this](OpenScopeSettings::WorkspaceView view)
        {
            settingsService_->update(
                [view](OpenScopeSettings& settings)
                {
                    settings.local.workspace.view =
                        view;
                });

            switch (view)
            {
            case OpenScopeSettings::WorkspaceView::Video:
                activeRenderView_ = RenderView::Video;
                break;

            case OpenScopeSettings::WorkspaceView::Waveform:
                activeRenderView_ = RenderView::Waveform;
                break;

            case OpenScopeSettings::WorkspaceView::Vectorscope:
                activeRenderView_ = RenderView::Vectorscope;
                break;

            case OpenScopeSettings::WorkspaceView::Matrix:
            case OpenScopeSettings::WorkspaceView::Headless:
            default:
                activeRenderView_ = RenderView::Matrix;
                break;
            }

            if (configAction_ != nullptr)
            {
                configAction_->setEnabled(
                    workspace_ != nullptr &&
                    workspace_->hasMaximizedViewport());
            }

            updateScreenRenderDemand();
            updateRenderResolutionTitle();
        });

    connect(
        workspace_,
        &ScopeWorkspace::videoMaximizedChanged,
        this,
        [this](bool maximized)
        {
            updateVideoFullscreenUi(
                maximized);
        });

    const OpenScopeSettings::WorkspaceView initialWorkspaceView =
        initialSettings.local
            .workspace
            .view;

    switch (initialWorkspaceView)
    {
    case OpenScopeSettings::WorkspaceView::Video:
        activeRenderView_ = RenderView::Video;
        break;
    case OpenScopeSettings::WorkspaceView::Waveform:
        activeRenderView_ = RenderView::Waveform;
        break;
    case OpenScopeSettings::WorkspaceView::Vectorscope:
        activeRenderView_ = RenderView::Vectorscope;
        break;
    case OpenScopeSettings::WorkspaceView::Matrix:
    case OpenScopeSettings::WorkspaceView::Headless:
    default:
        activeRenderView_ = RenderView::Matrix;
        break;
    }

    updateScreenRenderDemand();

    workspace_->setWorkspaceView(
        initialWorkspaceView);

    if (windowSettings.maximized)
    {
        showMaximized();
    }

    performanceTimer_ =
        new QTimer(this);

    performanceTimer_->setInterval(50);

    connect(
        performanceTimer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            performanceWidget_->setPerformanceSnapshot(
                videoEngine_->performanceSnapshot());
        });

    performanceTimer_->start();

    updateVideoFullscreenUi(
        workspace_->isVideoMaximized());

    updateRenderResolutionTitle();
}


void MainWindow::homeFloaties()
{
    QScreen* screen = nullptr;

    if (QWindow* const handle = windowHandle())
    {
        screen = handle->screen();
    }

    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }

    if (screen == nullptr)
    {
        return;
    }

    const QRect available =
        screen->availableGeometry();

    const QPoint home =
        available.topLeft() +
        QPoint(24, 24);

    // Cascade the floaties slightly so none of them disappears exactly
    // underneath another. Their sizes and visibility are left untouched.
    if (ySpectrumWindow_ != nullptr)
    {
        ySpectrumWindow_->move(
            home);
    }

    if (performanceWidget_ != nullptr)
    {
        performanceWidget_->move(
            home + QPoint(48, 48));
    }

    if (workspace_ != nullptr)
    {
        workspace_->homeFloatingSettings(
            home + QPoint(96, 96));
    }
}

void MainWindow::createSourceMenu()
{
    QMenu* sourceMenu =
        menuBar()->addMenu(
            tr("Source"));

    // Keep Config reachable while a single scope occupies the workspace,
    // without automatically throwing the Settings tool window over it.
    configAction_ =
        menuBar()->addAction(
            tr("Config (F2)"));

    // In quad/Matrix mode Settings is already the fourth viewport and F2 is
    // deliberately ignored. Reflect that in the menu bar instead of leaving
    // Config looking actionable. It is enabled again as soon as a scope is
    // maximized.
    configAction_->setEnabled(
        workspace_ != nullptr &&
        workspace_->hasMaximizedViewport());

    connect(
        configAction_,
        &QAction::triggered,
        this,
        [this]()
        {
            if (workspace_ != nullptr)
            {
                workspace_->toggleSettingsWindow();
            }
        });

    sourceGroup_ =
        new QActionGroup(this);

    sourceGroup_->setExclusive(true);

    blackmagicSourceMenu_ =
        sourceMenu->addMenu(
            tr("Blackmagic"));

    // Device enumeration is deliberately deferred.  The startup probe is
    // queued after the main window has entered the event loop; probing here
    // would reintroduce the old Desktop Video startup stall.
    blackmagicSourceMenu_->setEnabled(false);

    philipsPatternRomSourceAction_ =
        sourceMenu->addAction(
            tr("Philips Pattern ROM..."));

    philipsPatternRomSourceAction_->setCheckable(true);
    sourceGroup_->addAction(
        philipsPatternRomSourceAction_);

    imageFileSourceAction_ =
        sourceMenu->addAction(
            tr("Image file..."));
    imageFileSourceAction_->setCheckable(true);
    sourceGroup_->addAction(
        imageFileSourceAction_);

    sourceMenu->addSeparator();

    reloadPhilipsPatternRomAction_ =
        sourceMenu->addAction(
            tr("Reload Philips ROM set"));

    reloadPhilipsPatternRomAction_->setEnabled(false);

    connect(
        philipsPatternRomSourceAction_,
        &QAction::triggered,
        this,
        [this]()
        {
            selectPhilipsPatternRomSource();
        });

    connect(
        imageFileSourceAction_,
        &QAction::triggered,
        this,
        [this]()
        {
            selectImageFileSource();
        });

    connect(
        reloadPhilipsPatternRomAction_,
        &QAction::triggered,
        this,
        [this]()
        {
            reloadPhilipsPatternRomSource();
        });
}

void MainWindow::refreshBlackmagicDeviceMenu()
{
    if (blackmagicSourceMenu_ == nullptr ||
        sourceGroup_ == nullptr)
    {
        return;
    }

    blackmagicSourceMenu_->clear();
    blackmagicSourceAction_ = nullptr;

    const QStringList deviceNames =
        deckLinkDeviceNames();

    blackmagicSourceMenu_->setEnabled(
        !deviceNames.isEmpty());

    for (int index = 0; index < deviceNames.size(); ++index)
    {
        QAction* action =
            blackmagicSourceMenu_->addAction(
                deviceNames.at(index));

        action->setCheckable(true);
        sourceGroup_->addAction(action);

        if (index == selectedBlackmagicDeviceIndex_)
        {
            blackmagicSourceAction_ = action;
            action->setChecked(true);
        }

        connect(
            action,
            &QAction::triggered,
            this,
            [this, index]()
            {
                selectBlackmagicSource(index);
            });
    }
}

void MainWindow::selectBlackmagicSource(
    int deviceIndex)
{
    selectedBlackmagicDeviceIndex_ =
        (std::max)(0, deviceIndex);

    // The device choice is local machine state, just like the other Local/*
    // settings.  Persist it immediately so the next deferred startup probe
    // opens the same DeckLink card again.
    {
        const QString settingsFileName =
            QDir(
                QCoreApplication::applicationDirPath())
                .filePath(
                    QStringLiteral(
                        "OpenScope.ini"));

        QSettings settings(
            settingsFileName,
            QSettings::IniFormat);

        settings.setValue(
            QStringLiteral(
                "Local/Blackmagic/DeviceIndex"),
            selectedBlackmagicDeviceIndex_);
    }

    deckLinkSetPreferredDeviceIndex(
        selectedBlackmagicDeviceIndex_);

    if (philipsPatternRomSource_ != nullptr)
    {
        philipsPatternRomSource_->stop();
    }
    if (imageSourceTimer_ != nullptr)
    {
        imageSourceTimer_->stop();
    }

    waveformWidget_->setInputSampleClockHz(13'500'000.0);
    ySpectrumWindow_->setSnrMeasurementEnabled(true);
    waveformWidget_->setSnrMeasurementEnabled(true);

    blackmagicSourceActive_ = true;
    videoEngine_->setLumaCompensationEnabled(
        settingsService_->settings()
            .control
            .processing
            .lumaCompensation
            .enabled);

    deckLinkStop();
    setBlackmagicDeviceName(
        deckLinkProbe(
            videoEngine_,
            selectedBlackmagicDeviceIndex_));

    if (blackmagicSourceAction_ != nullptr)
    {
        blackmagicSourceAction_->setChecked(true);
    }
}

void MainWindow::setBlackmagicDeviceName(
    const QString& deviceName)
{
    const DeckLinkCompositeGainState gainState =
        deckLinkCompositeGainState();

    if (workspace_ != nullptr)
    {
        workspace_->setCompositeInputGainState(
            gainState.lumaAvailable,
            gainState.chromaAvailable,
            static_cast<int>(std::lround(gainState.minimumDb * 100.0)),
            static_cast<int>(std::lround(gainState.maximumDb * 100.0)),
            static_cast<int>(std::lround(gainState.lumaDb * 100.0)),
            static_cast<int>(std::lround(gainState.chromaDb * 100.0)));
    }

    blackmagicDeviceName_ =
        deviceName.isEmpty()
        ? QStringLiteral("BMD")
        : deviceName;

    refreshBlackmagicDeviceMenu();

    if (blackmagicSourceMenu_ != nullptr)
    {
        blackmagicSourceMenu_->setEnabled(
            !deviceName.isEmpty() &&
            !deckLinkDeviceNames().isEmpty());
    }

    if (blackmagicSourceAction_ != nullptr)
    {
        blackmagicSourceAction_->setChecked(
            !deviceName.isEmpty());
    }

    VectorscopePresentationInfo vectorscopePresentation;
    vectorscopePresentation.source = blackmagicDeviceName_;
    vectorscopePresentation.input = QStringLiteral("Composite");
    vectorscopePresentation.standard = QStringLiteral("625/50d");
    vectorscopePresentation.targets =
        settingsService_->settings()
            .control
            .instrument
            .vectorscope
            .showHundredPercentTargets
        ? QStringLiteral("100%")
        : QStringLiteral("75%");
    vectorscopePresentation.matrix = QStringLiteral("BT.601");
    vectorscopePresentation.processing = QStringLiteral("YUV 4:2:2 10 bit");

    videoEngine_->setVectorscopePresentationInfo(
        vectorscopePresentation);
}

void MainWindow::selectPhilipsPatternRomSource()
{
    const QString settingsFileName =
        QDir(
            QCoreApplication::applicationDirPath())
            .filePath(
                QStringLiteral("OpenScope.ini"));

    QSettings settings(
        settingsFileName,
        QSettings::IniFormat);

    QString initialPath =
        settings.value(
            QStringLiteral(
                "Local/PhilipsPatternRom/LastIni"))
            .toString();

    if (initialPath.isEmpty())
    {
        initialPath =
            QCoreApplication::applicationDirPath();
    }

    const QString iniFileName =
        QFileDialog::getOpenFileName(
            this,
            tr("Open Philips Pattern ROM set"),
            initialPath,
            tr("ROM set (rom.ini *.ini);;INI files (*.ini);;All files (*.*)"));

    if (iniFileName.isEmpty())
    {
        if (philipsPatternRomSource_ == nullptr ||
            !philipsPatternRomSource_->isRunning())
        {
            if (blackmagicSourceAction_ != nullptr)
            {
                blackmagicSourceAction_->setChecked(true);
            }
        }

        return;
    }

    QString errorMessage;

    if (!philipsPatternRomSource_->load(
            iniFileName,
            &errorMessage))
    {
        QMessageBox::warning(
            this,
            tr("Philips Pattern ROM"),
            errorMessage);

        if (blackmagicSourceAction_ != nullptr)
        {
            blackmagicSourceAction_->setChecked(true);
        }
        ySpectrumWindow_->setSnrMeasurementEnabled(true);
        waveformWidget_->setSnrMeasurementEnabled(true);
        blackmagicSourceActive_ = true;
        videoEngine_->setLumaCompensationEnabled(
                settingsService_->settings()
                        .control
                        .processing
                        .lumaCompensation
                        .enabled);

        deckLinkStop();
        setBlackmagicDeviceName(
            deckLinkProbe(videoEngine_, selectedBlackmagicDeviceIndex_));
        return;
    }

    settings.setValue(
        QStringLiteral(
            "Local/PhilipsPatternRom/LastIni"),
        iniFileName);

    waveformWidget_->setInputSampleClockHz(
        philipsPatternRomSource_->lumaSampleRateHz());

    ySpectrumWindow_->setSnrMeasurementEnabled(
        false,
        QStringLiteral("DIGITAL ROM SOURCE   SNR not applicable"));
    waveformWidget_->setSnrMeasurementEnabled(false);

    if (imageSourceTimer_ != nullptr)
    {
        imageSourceTimer_->stop();
    }

    blackmagicSourceActive_ = false;
    videoEngine_->setLumaCompensationEnabled(false);

    deckLinkStop();

    if (workspace_ != nullptr)
    {
        workspace_->setCompositeInputGainState(
            false,
            false,
            0,
            0,
            0,
            0);
    }

    philipsPatternRomSource_->start();

    VectorscopePresentationInfo vectorscopePresentation;
    vectorscopePresentation.source = QStringLiteral("Philips ROM");
    vectorscopePresentation.input = philipsPatternRomSource_->shortName();
    vectorscopePresentation.standard = QStringLiteral("625/50d");
    vectorscopePresentation.targets =
        settingsService_->settings()
            .control
            .instrument
            .vectorscope
            .showHundredPercentTargets
        ? QStringLiteral("100%")
        : QStringLiteral("75%");
    vectorscopePresentation.matrix = QStringLiteral("BT.601");
    vectorscopePresentation.processing = QStringLiteral("YUV 4:2:2 10 bit");

    videoEngine_->setVectorscopePresentationInfo(
        vectorscopePresentation);

    philipsPatternRomSourceAction_->setText(
        tr("Philips Pattern ROM - %1")
            .arg(
                philipsPatternRomSource_->setName()));

    philipsPatternRomSourceAction_->setChecked(true);
    reloadPhilipsPatternRomAction_->setEnabled(true);
}

void MainWindow::selectImageFileSource()
{
    const QString settingsFileName =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("OpenScope.ini"));
    QSettings settings(settingsFileName, QSettings::IniFormat);

    QString initialPath =
        settings.value(
            QStringLiteral("Local/ImageFile/LastPath"),
            QCoreApplication::applicationDirPath())
            .toString();

    const QString fileName =
        QFileDialog::getOpenFileName(
            this,
            tr("Open image"),
            initialPath,
            tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp);;All files (*.*)"));

    if (fileName.isEmpty())
    {
        if (imageSourceTimer_ != nullptr && imageSourceTimer_->isActive())
        {
            imageFileSourceAction_->setChecked(true);
        }
        else if (philipsPatternRomSource_ != nullptr && philipsPatternRomSource_->isRunning())
        {
            philipsPatternRomSourceAction_->setChecked(true);
        }
        else if (blackmagicSourceAction_ != nullptr)
        {
            blackmagicSourceAction_->setChecked(true);
        }
        return;
    }

    openImageFile(fileName);
}

bool MainWindow::openImageFile(const QString& fileName)
{
    auto newFrame = std::make_unique<Yuv444Frame>();
    QString errorMessage;
    QSize sourceImageSize;

    if (!imageToPalYuv444(
            fileName,
            *newFrame,
            &errorMessage,
            &sourceImageSize))
    {
        QMessageBox::warning(
            this,
            tr("Image file"),
            errorMessage);
        return false;
    }

    if (philipsPatternRomSource_ != nullptr)
    {
        philipsPatternRomSource_->stop();
    }
    deckLinkStop();

    blackmagicSourceActive_ = false;
    videoEngine_->setLumaCompensationEnabled(false);

    imageSourceFrame_ = std::move(newFrame);
    imageSourceFileName_ = fileName;

    waveformWidget_->setInputSampleClockHz(13'500'000.0);
    ySpectrumWindow_->setSnrMeasurementEnabled(
        false,
        QStringLiteral("STILL IMAGE SOURCE   SNR not applicable"));
    waveformWidget_->setSnrMeasurementEnabled(false);

    if (workspace_ != nullptr)
    {
        workspace_->setCompositeInputGainState(
            false, false, 0, 0, 0, 0);
    }

    const QString settingsFileName =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("OpenScope.ini"));
    QSettings settings(settingsFileName, QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("Local/ImageFile/LastPath"),
        QFileInfo(fileName).absolutePath());

    VectorscopePresentationInfo vectorscopePresentation;
    vectorscopePresentation.source = QStringLiteral("Image");
    vectorscopePresentation.input =
        sourceImageSize.isValid()
        ? QStringLiteral("%1x%2")
            .arg(sourceImageSize.width())
            .arg(sourceImageSize.height())
        : QStringLiteral("IMAGE");
    vectorscopePresentation.standard = QStringLiteral("625/50d");
    vectorscopePresentation.targets =
        settingsService_->settings()
            .control.instrument.vectorscope.showHundredPercentTargets
        ? QStringLiteral("100%")
        : QStringLiteral("75%");
    vectorscopePresentation.matrix = QStringLiteral("BT.601");
    vectorscopePresentation.processing = QStringLiteral("RGB file -> YUV 4:4:4");
    videoEngine_->setVectorscopePresentationInfo(vectorscopePresentation);

    if (imageFileSourceAction_ != nullptr)
    {
        imageFileSourceAction_->setText(
            tr("Image - %1").arg(QFileInfo(fileName).fileName()));
        imageFileSourceAction_->setChecked(true);
    }
    if (reloadPhilipsPatternRomAction_ != nullptr)
    {
        reloadPhilipsPatternRomAction_->setEnabled(false);
    }

    publishImageFrame();
    if (imageSourceTimer_ != nullptr)
    {
        imageSourceTimer_->start();
    }
    return true;
}

void MainWindow::publishImageFrame()
{
    if (imageSourceFrame_ == nullptr || videoEngine_ == nullptr)
    {
        return;
    }

    if (Yuv444Frame* destination = videoEngine_->tryAcquireWriteFrame())
    {
        *destination = *imageSourceFrame_;
        videoEngine_->submitWriteFrame();
    }
}

void MainWindow::reloadPhilipsPatternRomSource()
{
    if (philipsPatternRomSource_ == nullptr ||
        philipsPatternRomSource_->iniFileName().isEmpty())
    {
        return;
    }

    const QString iniFileName =
        philipsPatternRomSource_->iniFileName();

    QString errorMessage;

    if (!philipsPatternRomSource_->load(
            iniFileName,
            &errorMessage))
    {
        QMessageBox::warning(
            this,
            tr("Philips Pattern ROM"),
            errorMessage);

        if (blackmagicSourceAction_ != nullptr)
        {
            blackmagicSourceAction_->setChecked(true);
        }
        ySpectrumWindow_->setSnrMeasurementEnabled(true);
        waveformWidget_->setSnrMeasurementEnabled(true);
        blackmagicSourceActive_ = true;
        videoEngine_->setLumaCompensationEnabled(
                settingsService_->settings()
                        .control
                        .processing
                        .lumaCompensation
                        .enabled);

        setBlackmagicDeviceName(
            deckLinkProbe(videoEngine_, selectedBlackmagicDeviceIndex_));
        return;
    }

    waveformWidget_->setInputSampleClockHz(
        philipsPatternRomSource_->lumaSampleRateHz());

    ySpectrumWindow_->setSnrMeasurementEnabled(
        false,
        QStringLiteral("DIGITAL ROM SOURCE   SNR not applicable"));
    waveformWidget_->setSnrMeasurementEnabled(false);

    if (imageSourceTimer_ != nullptr)
    {
        imageSourceTimer_->stop();
    }

    blackmagicSourceActive_ = false;
    videoEngine_->setLumaCompensationEnabled(false);

    deckLinkStop();

    if (workspace_ != nullptr)
    {
        workspace_->setCompositeInputGainState(
            false,
            false,
            0,
            0,
            0,
            0);
    }

    philipsPatternRomSource_->start();

    VectorscopePresentationInfo vectorscopePresentation;
    vectorscopePresentation.source = QStringLiteral("Philips ROM");
    vectorscopePresentation.input = philipsPatternRomSource_->shortName();
    vectorscopePresentation.standard = QStringLiteral("625/50d");
    vectorscopePresentation.targets =
        settingsService_->settings()
            .control
            .instrument
            .vectorscope
            .showHundredPercentTargets
        ? QStringLiteral("100%")
        : QStringLiteral("75%");
    vectorscopePresentation.matrix = QStringLiteral("BT.601");
    vectorscopePresentation.processing = QStringLiteral("YUV 4:2:2 10 bit");

    videoEngine_->setVectorscopePresentationInfo(
        vectorscopePresentation);

    philipsPatternRomSourceAction_->setText(
        tr("Philips Pattern ROM - %1")
            .arg(
                philipsPatternRomSource_->setName()));

    philipsPatternRomSourceAction_->setChecked(true);
}

double MainWindow::windowAspectRatio() const
{
    if (settingsService_ != nullptr &&
        settingsService_->settings()
            .local
            .display
            .aspectRatio ==
        OpenScopeSettings::AspectRatio::Ratio4x3)
    {
        return
            OpenScopeSettings::aspectRatioValue(
                OpenScopeSettings::AspectRatio::Ratio4x3);
    }

    return
        OpenScopeSettings::aspectRatioValue(
            OpenScopeSettings::AspectRatio::Ratio16x9);
}

void MainWindow::applyDisplayAspectRatio(
    OpenScopeSettings::AspectRatio aspectRatio,
    bool resizeWindow)
{
    videoWidget_->setAspectRatio(
        aspectRatio);

    waveformWidget_->setAspectRatio(
        aspectRatio);

    vectorscopeWidget_->setAspectRatio(
        aspectRatio);

    videoEngine_->setWaveformAspectRatio(
        aspectRatio);

    if (workspace_ != nullptr)
    {
        workspace_->setAspectRatio(
            aspectRatio);
    }

    if (!resizeWindow)
    {
        return;
    }

    const double aspect =
        OpenScopeSettings::aspectRatioValue(
            aspectRatio);

    // F11 is a true monitor-sized fullscreen state.  Do not apply the normal
    // top-level aspect-ratio resize while fullscreen: that leaves Qt/Windows
    // with stale fullscreen geometry.  Update the geometry we will restore to,
    // then re-assert fullscreen on the next event turn so every child layout
    // is recomputed for the new display aspect ratio.
    if (f11FullScreen_)
    {
        if (f11RestoreWindowGeometry_.isValid())
        {
            const int restoredHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(
                            f11RestoreWindowGeometry_.width()) /
                        aspect));

            f11RestoreWindowGeometry_.setHeight(restoredHeight);
        }

        QTimer::singleShot(
            0,
            this,
            [this]()
            {
                if (!f11FullScreen_)
                {
                    return;
                }

                showFullScreen();

                if (workspace_ != nullptr)
                {
                    workspace_->updateGeometry();
                    workspace_->update();
                }

                if (centralWidget() != nullptr)
                {
                    centralWidget()->updateGeometry();
                    centralWidget()->update();
                }
            });

        return;
    }

    // If the OpenScope window is in our aspect-ratio-constrained
    // custom maximized state, two geometries must be updated:
    //
    // 1. The saved normal geometry, otherwise restoring from
    //    maximized uses the aspect ratio that was active before
    //    the switch.
    // 2. The currently maximized geometry itself. It must be fit
    //    to the monitor work area using the new aspect ratio.
    if (customMaximized_)
    {
        if (restoreWindowGeometry_.isValid())
        {
            const int restoredHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(
                            restoreWindowGeometry_.width()) /
                        aspect));

            restoreWindowGeometry_.setHeight(
                restoredHeight);
        }

        const HWND hwnd =
            reinterpret_cast<HWND>(
                winId());

        const HMONITOR monitor =
            MonitorFromWindow(
                hwnd,
                MONITOR_DEFAULTTONEAREST);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize =
            sizeof(MONITORINFO);

        if (GetMonitorInfo(
            monitor,
            &monitorInfo))
        {
            const RECT& workArea =
                monitorInfo.rcWork;

            const int availableWidth =
                workArea.right -
                workArea.left;

            const int availableHeight =
                workArea.bottom -
                workArea.top;

            RECT currentWindowRect{};
            GetWindowRect(
                hwnd,
                &currentWindowRect);

            const int currentWindowWidth =
                static_cast<int>(
                    currentWindowRect.right -
                    currentWindowRect.left);

            const int currentWindowHeight =
                static_cast<int>(
                    currentWindowRect.bottom -
                    currentWindowRect.top);

            const int chromeWidth =
                workspace_ != nullptr
                    ? (std::max)(
                        0,
                        currentWindowWidth -
                            workspace_->width())
                    : 0;

            const int chromeHeight =
                workspace_ != nullptr
                    ? (std::max)(
                        0,
                        currentWindowHeight -
                            workspace_->height())
                    : 0;

            int workspaceWidth =
                (std::max)(1, availableWidth - chromeWidth);

            int workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        aspect));

            if (workspaceHeight + chromeHeight > availableHeight)
            {
                workspaceHeight =
                    (std::max)(1, availableHeight - chromeHeight);

                workspaceWidth =
                    static_cast<int>(
                        std::lround(
                            static_cast<double>(workspaceHeight) *
                            aspect));
            }

            const int windowWidth =
                workspaceWidth + chromeWidth;

            const int windowHeight =
                workspaceHeight + chromeHeight;

            const int x =
                workArea.left +
                (availableWidth -
                    windowWidth) / 2;

            const int y =
                workArea.top +
                (availableHeight -
                    windowHeight) / 2;

            SetWindowPos(
                hwnd,
                nullptr,
                x,
                y,
                windowWidth,
                windowHeight,
                SWP_NOZORDER |
                SWP_NOACTIVATE);

            return;
        }
    }

    // QWidget::width()/height() describe the client area, while the
    // ScopeWorkspace excludes the menu bar.  Preserve that fixed client
    // chrome and apply the selected aspect ratio to the workspace only.
    const int clientChromeWidth =
        workspace_ != nullptr
            ? (std::max)(0, width() - workspace_->width())
            : 0;

    const int clientChromeHeight =
        workspace_ != nullptr
            ? (std::max)(0, height() - workspace_->height())
            : 0;

    const int workspaceWidth =
        (std::max)(1, width() - clientChromeWidth);

    const int newWorkspaceHeight =
        static_cast<int>(
            std::lround(
                static_cast<double>(workspaceWidth) /
                aspect));

    resize(
        workspaceWidth + clientChromeWidth,
        newWorkspaceHeight + clientChromeHeight);
}

void MainWindow::updateVideoFullscreenUi(
    bool fullscreen)
{
    videoEngine_->setVideoHighlightEnabled(
        !fullscreen);
}

void MainWindow::updateScreenRenderDemand()
{
    bool videoScreen = false;
    bool waveformScreen = false;
    bool vectorscopeScreen = false;

    switch (activeRenderView_)
    {
    case RenderView::Video:
        videoScreen = true;
        break;

    case RenderView::Waveform:
        waveformScreen = true;
        break;

    case RenderView::Vectorscope:
        vectorscopeScreen = true;
        break;

    case RenderView::Matrix:
    default:
        videoScreen = true;
        waveformScreen = true;
        vectorscopeScreen = true;
        break;
    }

    videoEngine_->setVideoScreenRenderEnabled(
        videoScreen);

    videoEngine_->setWaveformScreenRenderEnabled(
        waveformScreen);

    // When the user explicitly swaps the vectorscope viewport for the
    // PCM Audio Scope, the normal on-screen chroma vectorscope has no
    // consumer. Spout remains independently gated by its own consumer.
    videoEngine_->setVectorscopeScreenRenderEnabled(
        vectorscopeScreen && !pcmAudioScopeActive_);
}


void MainWindow::updateRenderResolutionTitle()
{
    QSize renderSize;

    switch (activeRenderView_)
    {
    case RenderView::Video:
        renderSize =
            videoRenderSize_;
        break;

    case RenderView::Waveform:
        renderSize =
            waveformRenderSize_;
        break;

    case RenderView::Vectorscope:
        renderSize =
            vectorscopeRenderSize_;
        break;

    case RenderView::Matrix:
    default:
        // In matrix mode the video viewport is our
        // render-resolution reference.
        renderSize =
            videoRenderSize_;
        break;
    }

    const bool releaseBuild =
        QStringLiteral(OPENSCOPE_DELTA) == QStringLiteral("0");

    if (renderSize.isValid())
    {
        if (releaseBuild)
        {
            setWindowTitle(
                QString("OpenScope V" OPENSCOPE_VERSION " - %1x%2")
                    .arg(renderSize.width())
                    .arg(renderSize.height()));
        }
        else
        {
            setWindowTitle(
                QString(
                    "OpenScope V" OPENSCOPE_VERSION " - %1x%2 - Delta " OPENSCOPE_DELTA)
                .arg(renderSize.width())
                .arg(renderSize.height()));
        }
    }
    else
    {
        setWindowTitle(
            releaseBuild
            ? QStringLiteral("OpenScope V" OPENSCOPE_VERSION)
            : QStringLiteral(
                "OpenScope V" OPENSCOPE_VERSION " - Delta " OPENSCOPE_DELTA));
    }
}

VideoWidget* MainWindow::videoWidget() const
{
    return videoWidget_;
}

MainWindow::~MainWindow()
{
#ifdef Q_OS_WIN
    timeEndPeriod(1);
#endif

    if (preventDisplaySleepActive_)
    {
        SetThreadExecutionState(ES_CONTINUOUS);
        preventDisplaySleepActive_ = false;
    }

    if (imageSourceTimer_ != nullptr)
    {
        imageSourceTimer_->stop();
    }

    if (philipsPatternRomSource_ != nullptr)
    {
        philipsPatternRomSource_->stop();
    }

    deckLinkStop();
}

VideoEngine* MainWindow::videoEngine() const
{
    return videoEngine_;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event != nullptr &&
        event->mimeData() != nullptr &&
        event->mimeData()->hasUrls())
    {
        for (const QUrl& url : event->mimeData()->urls())
        {
            if (url.isLocalFile())
            {
                event->acceptProposedAction();
                return;
            }
        }
    }

    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (event != nullptr &&
        event->mimeData() != nullptr)
    {
        for (const QUrl& url : event->mimeData()->urls())
        {
            if (url.isLocalFile() && openImageFile(url.toLocalFile()))
            {
                event->acceptProposedAction();
                return;
            }
        }
    }

    QMainWindow::dropEvent(event);
}

bool MainWindow::nativeEvent(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
    const MSG* msg =
        static_cast<const MSG*>(
            message);

    if (msg->message == WM_SYSCOMMAND &&
        (msg->wParam & 0xFFF0) == SC_MAXIMIZE)
    {
        if (customMaximized_)
        {
            setGeometry(
                restoreWindowGeometry_);

            customMaximized_ = false;

            *result = 0;
            return true;
        }
        const HWND hwnd =
            reinterpret_cast<HWND>(
                winId());

        const HMONITOR monitor =
            MonitorFromWindow(
                hwnd,
                MONITOR_DEFAULTTONEAREST);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize =
            sizeof(MONITORINFO);

        if (GetMonitorInfo(
            monitor,
            &monitorInfo))
        {
            const RECT& workArea =
                monitorInfo.rcWork;

            const int availableWidth =
                workArea.right -
                workArea.left;

            const int availableHeight =
                workArea.bottom -
                workArea.top;

            RECT currentWindowRect{};
            GetWindowRect(
                hwnd,
                &currentWindowRect);

            const int currentWindowWidth =
                static_cast<int>(
                    currentWindowRect.right -
                    currentWindowRect.left);

            const int currentWindowHeight =
                static_cast<int>(
                    currentWindowRect.bottom -
                    currentWindowRect.top);

            const int chromeWidth =
                workspace_ != nullptr
                    ? (std::max)(
                        0,
                        currentWindowWidth -
                            workspace_->width())
                    : 0;

            const int chromeHeight =
                workspace_ != nullptr
                    ? (std::max)(
                        0,
                        currentWindowHeight -
                            workspace_->height())
                    : 0;

            const double aspect =
                windowAspectRatio();

            int workspaceWidth =
                (std::max)(1, availableWidth - chromeWidth);

            int workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        aspect));

            if (workspaceHeight + chromeHeight > availableHeight)
            {
                workspaceHeight =
                    (std::max)(1, availableHeight - chromeHeight);

                workspaceWidth =
                    static_cast<int>(
                        std::lround(
                            static_cast<double>(workspaceHeight) *
                            aspect));
            }

            const int windowWidth =
                workspaceWidth + chromeWidth;

            const int windowHeight =
                workspaceHeight + chromeHeight;

            const int x =
                workArea.left +
                (availableWidth -
                    windowWidth) / 2;

            const int y =
                workArea.top +
                (availableHeight -
                    windowHeight) / 2;

            restoreWindowGeometry_ =
                geometry();

            customMaximized_ = true;

            SetWindowPos(
                hwnd,
                nullptr,
                x,
                y,
                windowWidth,
                windowHeight,
                SWP_NOZORDER |
                SWP_NOACTIVATE);
        }

        *result = 0;
        return true;
    }

    if (msg->message == WM_SIZING)
    {
        RECT* const rect =
            reinterpret_cast<RECT*>(
                msg->lParam);

        const HWND hwnd =
            reinterpret_cast<HWND>(
                winId());

        const HMONITOR monitor =
            MonitorFromRect(
                rect,
                MONITOR_DEFAULTTONEAREST);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize =
            sizeof(MONITORINFO);

        if (!GetMonitorInfo(
            monitor,
            &monitorInfo))
        {
            return QMainWindow::nativeEvent(
                eventType,
                message,
                result);
        }

        // WM_SIZING supplies the OUTER Windows rectangle.  The video/scope
        // workspace is smaller because that rectangle also contains the
        // native frame/title bar and the Qt menu bar (Source).  Measure that
        // fixed chrome from the live window instead of guessing pixel counts;
        // this automatically follows DPI, Windows theme and menu font size.
        RECT currentWindowRect{};
        GetWindowRect(
            hwnd,
            &currentWindowRect);

        const int currentOuterWidth =
            currentWindowRect.right -
            currentWindowRect.left;

        const int currentOuterHeight =
            currentWindowRect.bottom -
            currentWindowRect.top;

        const int currentWorkspaceWidth =
            workspace_ != nullptr
                ? workspace_->width()
                : width();

        const int currentWorkspaceHeight =
            workspace_ != nullptr
                ? workspace_->height()
                : height();

        const int chromeWidth =
            (std::max)(
                0,
                currentOuterWidth -
                    currentWorkspaceWidth);

        const int chromeHeight =
            (std::max)(
                0,
                currentOuterHeight -
                    currentWorkspaceHeight);

        const double aspect =
            windowAspectRatio();

        int outerWidth =
            rect->right -
            rect->left;

        int outerHeight =
            rect->bottom -
            rect->top;

        int workspaceWidth =
            (std::max)(
                1,
                outerWidth - chromeWidth);

        int workspaceHeight =
            (std::max)(
                1,
                outerHeight - chromeHeight);

        switch (msg->wParam)
        {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        aspect));
            break;

        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            workspaceWidth =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceHeight) *
                        aspect));
            break;

        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
        case WMSZ_BOTTOMLEFT:
        case WMSZ_BOTTOMRIGHT:
            // Keep the existing OpenScope behaviour for corner drags: width
            // is authoritative, but apply the ratio to the workspace rather
            // than to the complete decorated window.
            workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        aspect));
            break;

        default:
            break;
        }

        outerWidth =
            workspaceWidth +
            chromeWidth;

        outerHeight =
            workspaceHeight +
            chromeHeight;

        const RECT& workArea =
            monitorInfo.rcWork;

        const int maxOuterWidth =
            workArea.right -
            workArea.left;

        const int maxOuterHeight =
            workArea.bottom -
            workArea.top;

        if (outerWidth > maxOuterWidth)
        {
            workspaceWidth =
                (std::max)(
                    1,
                    maxOuterWidth - chromeWidth);

            workspaceHeight =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceWidth) /
                        aspect));

            outerWidth =
                workspaceWidth +
                chromeWidth;

            outerHeight =
                workspaceHeight +
                chromeHeight;
        }

        if (outerHeight > maxOuterHeight)
        {
            workspaceHeight =
                (std::max)(
                    1,
                    maxOuterHeight - chromeHeight);

            workspaceWidth =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(workspaceHeight) *
                        aspect));

            outerWidth =
                workspaceWidth +
                chromeWidth;

            outerHeight =
                workspaceHeight +
                chromeHeight;
        }

        switch (msg->wParam)
        {
        case WMSZ_LEFT:
        case WMSZ_TOPLEFT:
        case WMSZ_BOTTOMLEFT:
            rect->left =
                rect->right -
                outerWidth;
            break;

        default:
            rect->right =
                rect->left +
                outerWidth;
            break;
        }

        switch (msg->wParam)
        {
        case WMSZ_TOP:
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            rect->top =
                rect->bottom -
                outerHeight;
            break;

        default:
            rect->bottom =
                rect->top +
                outerHeight;
            break;
        }

        *result = TRUE;
        return true;
    }

    return QMainWindow::nativeEvent(
        eventType,
        message,
        result);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event != nullptr)
    {
        if (event->type() == QEvent::ApplicationActivate)
        {
            traceLog(TraceEventType::FocusGain);
        }
        else if (event->type() == QEvent::ApplicationDeactivate)
        {
            traceLog(TraceEventType::FocusLost);
        }
    }

    if (event != nullptr &&
        event->type() == QEvent::KeyPress)
    {
        auto* keyEvent =
            static_cast<QKeyEvent*>(event);

        const bool configPressed =
            keyEvent->key() == Qt::Key_F2 &&
            keyEvent->modifiers() == Qt::NoModifier;

        if (configPressed)
        {
            if (!keyEvent->isAutoRepeat() &&
                workspace_ != nullptr &&
                workspace_->hasMaximizedViewport())
            {
                workspace_->toggleSettingsWindow();
            }

            return true;
        }

        const bool f11Pressed =
            keyEvent->key() == Qt::Key_F11 &&
            keyEvent->modifiers() == Qt::NoModifier;

        const bool escapeFromF11FullScreen =
            f11FullScreen_ &&
            keyEvent->key() == Qt::Key_Escape &&
            keyEvent->modifiers() == Qt::NoModifier;

        if (f11Pressed ||
            escapeFromF11FullScreen)
        {
            // Ignore key-repeat so holding F11/Escape cannot cause
            // repeated fullscreen state changes.
            if (keyEvent->isAutoRepeat())
            {
                return true;
            }

            if (!f11FullScreen_)
            {
                // Preserve the exact current top-level geometry. This also
                // behaves correctly when OpenScope is in its custom
                // aspect-ratio maximized state.
                f11RestoreWindowGeometry_ =
                    geometry();

                f11MenuBarWasVisible_ =
                    menuBar() != nullptr &&
                    menuBar()->isVisible();

                if (menuBar() != nullptr)
                {
                    menuBar()->hide();
                }

                showFullScreen();
                f11FullScreen_ = true;

                // F11 uses the monitor as a windowless black canvas.  Leave
                // Matrix/quad geometry untouched; only refresh the video
                // presentation once fullscreen geometry has settled so the
                // selected 4:3/16:9 display aspect is maximally fitted and
                // any remainder stays black in VideoWidget::paintEvent().
                QTimer::singleShot(
                    0,
                    this,
                    [this]()
                    {
                        if (!f11FullScreen_)
                        {
                            return;
                        }

                        videoWidget_->refreshOutputSize();
                        videoWidget_->update();
                    });
            }
            else
            {
                showNormal();

                if (f11RestoreWindowGeometry_.isValid())
                {
                    setGeometry(
                        f11RestoreWindowGeometry_);
                }

                if (menuBar() != nullptr)
                {
                    menuBar()->setVisible(
                        f11MenuBarWasVisible_);
                }

                f11FullScreen_ = false;
            }

            return true;
        }

        if (keyEvent->key() == Qt::Key_F &&
            keyEvent->modifiers() == Qt::NoModifier)
        {
            ySpectrumWindow_->show();
            ySpectrumWindow_->raise();
            ySpectrumWindow_->activateWindow();

            return true;
        }
    }

    return QMainWindow::eventFilter(
        watched,
        event);
}

void MainWindow::closeEvent(
    QCloseEvent* event)
{
    if (lineNumberPersistTimer_ != nullptr)
    {
        lineNumberPersistTimer_->stop();
    }

    const QRect geometry =
        normalGeometry();

    const QPoint performancePosition =
        performanceWidget_ != nullptr
        ? performanceWidget_->pos()
        : QPoint();

    const bool performancePositionValid =
        performanceWidget_ != nullptr;

    const QPoint settingsPosition =
        workspace_ != nullptr
        ? workspace_->floatingSettingsPosition()
        : QPoint();

    const bool settingsPositionValid =
        workspace_ != nullptr &&
        workspace_->hasFloatingSettingsPosition();

    const QSize settingsSize =
        workspace_ != nullptr
        ? workspace_->floatingSettingsSize()
        : QSize();

    const bool settingsSizeValid =
        workspace_ != nullptr &&
        workspace_->hasFloatingSettingsSize();

    settingsService_->update(
        [&geometry,
         this,
         performancePosition,
         performancePositionValid,
         settingsPosition,
         settingsPositionValid,
         settingsSize,
         settingsSizeValid](OpenScopeSettings& settings)
        {
            settings.control
                .instrument
                .lineNumber =
                pendingLineNumber_;

            settings.local.window.x =
                geometry.x();

            settings.local.window.y =
                geometry.y();

            settings.local.window.width =
                geometry.width();

            settings.local.window.height =
                geometry.height();

            settings.local.window.maximized =
                isMaximized();

            if (performancePositionValid)
            {
                settings.local
                    .floaties
                    .performance
                    .x =
                    performancePosition.x();

                settings.local
                    .floaties
                    .performance
                    .y =
                    performancePosition.y();

                settings.local
                    .floaties
                    .performance
                    .positionValid =
                    true;
            }

            if (settingsPositionValid)
            {
                settings.local.floaties.settings.x =
                    settingsPosition.x();
                settings.local.floaties.settings.y =
                    settingsPosition.y();
                settings.local.floaties.settings.positionValid =
                    true;
            }

            if (settingsSizeValid)
            {
                settings.local.floaties.settings.width =
                    settingsSize.width();
                settings.local.floaties.settings.height =
                    settingsSize.height();
                settings.local.floaties.settings.sizeValid =
                    true;
            }
        });

    QMainWindow::closeEvent(event);
}