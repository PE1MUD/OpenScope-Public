#pragma once

#include <QObject>
#include <QImage>
#include <QByteArray>
#include <QThread>
#include <QVector>
#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "rendering/VectorscopeRenderer.h"
#include "analysis/WssDecoder.h"
#include "analysis/PcmVideoDecoder.h"
#include "analysis/HamPcmV2Decoder.h"
#include "output/WasapiPcmOutput.h"
#include "output/HamWasapiPcmOutput.h"
#include "output/AsioPcmOutput.h"
#include "processing/SignalReconstructor.h"
#include "processing/VideoDeinterlacer.h"
#include "processing/NoiseReducer.h"
#include "processing/LumaHighFrequencyCompensator.h"
#include "rendering/WaveformRenderer.h"
#include "util/PerformanceStats.h"
#include "video/DisplayConverter.h"
#include "video/ProgressiveLumaPair.h"
#include "video/ReconstructedLumaFrame.h"
#include "video/Yuv444Frame.h"
#include "settings/OpenScopeSettings.h"

class VideoEngine : public QObject
{
    Q_OBJECT

public:
    explicit VideoEngine(QObject* parent = nullptr);
    ~VideoEngine() override;

    Yuv444Frame* tryAcquireWriteFrame();
    void submitWriteFrame();
    void cancelWriteFrame();

    void setSelectedLine(int line);

    void requestWaveformFlatFieldSpectrum();

    void injectTestWss4x3();
    void injectTestWss16x9();

    bool startWaveformRawCapture(const std::string& rawFilePath);

    void setVideoOutputSize(
        int width,
        int height);

    void setDisplayGamma(
        double gamma);

    void setSpoutVideoEnabled(
        bool enabled);

    void setVideoScreenRenderEnabled(
        bool enabled);

    void setWaveformScreenRenderEnabled(
        bool enabled);

    void setVectorscopeScreenRenderEnabled(
        bool enabled);
    void setVectorscopeSuppressedForAudioScope(bool suppressed);

    void setVideoHighlightEnabled(
        bool enabled);

    void setVideoLineHighlightEnabled(
        bool enabled);

    void resetDisplayPresentation();

    void setNoiseReductionEnabled(
        bool enabled);

    void setPcmDecoderEnabled(
        bool enabled);

    void setPcmDecoderMode(
        int mode);

    void setPcmAudioOutputEnabled(
        bool enabled);

    void setPcmAudioOutputDevice(
        const std::string& endpointIdUtf8);

    void setPcmAudioOutputBackend(int backend);
    void setPcmAsioDriver(const std::string& driverNameUtf8);
    void setPcmAsioBufferLoggingEnabled(bool enabled);
    void setPcmAsioTargetMs(double milliseconds);
    double pcmAsioBufferPeriodMs() const noexcept { return asioPcmAudioOutput_.bufferPeriodMs(); }
    double pcmAsioTargetMs() const noexcept { return asioPcmAudioOutput_.targetMs(); }
    double pcmAsioQueuedMs() const noexcept { return asioPcmAudioOutput_.queuedMs(); }
    double pcmAsioLowWaterMs() const noexcept { return asioPcmAudioOutput_.lowWaterMs(); }
    double pcmAsioFastFillMs() const noexcept { return asioPcmAudioOutput_.fastFillMs(); }
    double pcmAsioAverage3sFillMs() const noexcept { return asioPcmAudioOutput_.average3sFillMs(); }
    double pcmAsioNudgePpm() const noexcept { return asioPcmAudioOutput_.nudgePpm(); }
    double pcmAsioSampleRate() const noexcept { return asioPcmAudioOutput_.asioSampleRate(); }
    long pcmAsioBufferFrames() const noexcept { return asioPcmAudioOutput_.bufferFrames(); }
    std::uint64_t pcmAsioUnderruns() const noexcept { return asioPcmAudioOutput_.underruns(); }
    std::uint64_t pcmAsioOverruns() const noexcept { return asioPcmAudioOutput_.overruns(); }
    std::uint64_t pcmAsioCallbackCount() const noexcept { return asioPcmAudioOutput_.callbackCount(); }

    void setPcmAudioBufferMs(
        int milliseconds);

    void setPcmMuteBottomTwoBits(
        bool enabled);

    void setPcmDeEmphasisMode(
        int mode);

    void setNoiseReductionIntensity(
        int intensity);

    void setLumaCompensationEnabled(
        bool enabled);

    void setLumaCompensationGainHundredthsDb(
        int gainHundredthsDb);

    void setLumaCompensationSourceEnabled(
        bool enabled);

    void setWaveformOutputSize(
        int width,
        int height);

    void setWaveformVideoContentScale(
        double scale);

    void setWaveformZoomed(
        bool zoomed);

    void setWaveformZoomFactor(
        int factor);

    void setWaveformScrollPosition(
        double position);

    void setWaveformPersistence(
        int persistence);

    void setWaveformCoreIntensity(
        int intensity);

    void setWaveformCoreWidth(
        int widthTenths);

    void setWaveformAntiAliasing(
        bool enabled);

    void setWaveformColorizeIllegalLuminance(
        bool enabled);

    void setVectorscopeColorizeGamutErrors(
        bool enabled);


    void setVectorscopeGlow(
        int glow);

    void setWaveformChromaFillIntensity(
        int intensity);

    void setWaveformColor(
        bool enabled);

    void setWaveformMeasurementProbePresentation(
        bool enabled,
        double normalizedX,
        double volts);

    void setVectorscopeOutputSize(
        int width,
        int height);

    void setVectorscopeVideoOutputSize(
        int width,
        int height);

    void setVectorscopeVideoContentScale(
        double horizontalScale,
        double verticalScale);

    void setVectorscopeVideoEnabled(bool enabled);

    void setWaveformVideoEnabled(bool enabled);

    void setVectorscopePresentationInfo(
        const VectorscopePresentationInfo& info);

    void setWaveformAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

    void setWaveformVideoAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

    PerformanceSnapshot performanceSnapshot() const;

    void recordVideoSpoutTiming(
        std::uint64_t queueDelayUs,
        std::uint64_t sendUs,
        std::uint64_t intervalUs);

    QImage captureHighResolutionSnapshot();

signals:
    void inputSignalStateChanged(bool valid);

    void frameChanged(
        const QImage& image);

    void videoSpoutChanged(
        const QImage& image,
        qint64 dispatchTimestampUs);

    void waveformChanged(
        const QImage& image);

    void waveformVideoChanged(
        const QImage& image);

    void waveformMeasurementDataChanged(
        const QVector<float>& samples);

    void waveformSpectrumDataChanged(
        const QVector<float>& fullLine,
        const QVector<float>& visiblePart,
        bool inputSignalValid);

    void waveformFlatFieldSpectrumDataChanged(
        const QVector<float>& samples,
        int lineLength,
        int lineCount,
        bool inputSignalValid);

    void vectorscopeChanged(
        const QImage& image);

    void vectorscopeVideoChanged(
        const QImage& image);

    void wssStateChanged(
        const QString& status,
        bool locked,
        int recommendedDisplayAspect);

    void pcmAudioLevelsChanged(float leftPeak, float rightPeak);
    void pcmAudioScopeSamplesChanged(const QByteArray& stereoPcm16);

    void pcmDecoderStatusChanged(
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

private:
    static constexpr unsigned int kLumaWorkerDivisor = 8;
    static constexpr unsigned int kMinimumWorkerCount = 2;

    static constexpr int kCaptureWidth = 720;
    static constexpr int kCaptureHeight = 576;

    static constexpr int kDefaultVideoWidth = 720;
    static constexpr int kDefaultVideoHeight = 576;

    static constexpr int kMinimumOutputSize = 1;
    static constexpr int kDefaultSelectedLine = 320;

    static constexpr std::size_t kFrameSlotCount = 3;
    static constexpr int kInvalidSlotIndex = -1;

    static constexpr int kLumaReconstructionRadius = 24;
    static constexpr float kLumaReconstructionCutoff = 1.00f;

    struct CapturedFrameSlot
    {
        std::atomic<std::uint64_t> generation{ 0 };
        std::atomic_bool writing{ false };
        std::atomic<std::int64_t> captureTickNs{ 0 };
        // Diagnostic time origin for this exact captured frame.  Capture
        // identity and timeline zero are deliberately separate: the
        // Performance floaty is a processing timeline, so 0 ms is when the
        // display pipeline starts, not when the DeckLink callback arrived.
        std::atomic<std::int64_t> diagnosticOriginNs{ 0 };
        // Capture-side Y frequency-compensation (F) duration for the
        // Performance wallclock.  Start is the frame diagnostic origin.
        std::atomic<std::uint32_t> frequencyCompensationUs{ 0 };

        // Display-only source line to blank when stable WSS is detected.
        // -1 means no WSS line should be suppressed. Raw capture data remains
        // untouched; this marker is consumed only by generated Video output.
        std::atomic<int> wssDisplayLine{ -1 };

        // Per-worker F chunks for this exact captured frame.  These are
        // Published independently to the four real worker lanes:
        // Display 1, Display 2, Waveform, Vectorscope.
        std::array<WaveformAssistTimelineStats, 2> frequencyAssistTimeline;

        std::atomic<std::uint64_t> vectorscopeGeneration{ 0 };
        Yuv444Frame vectorscopeFrame;

        std::atomic<std::uint64_t> waveformGeneration{ 0 };
        std::atomic<bool> waveformRawSelectedLine{ false };
        Yuv444Frame waveformFrame;

        Yuv444Frame frame;
    };

    struct PcmCaptureSlot
    {
        std::atomic<std::uint64_t> generation{ 0 };
        std::atomic_bool writing{ false };
        std::atomic_bool reading{ false };
        bool inputSignalValid = false;
        int width = 0;
        int height = 0;
        std::vector<std::uint16_t> y;
    };

    struct DisplayFrameSlot
    {
        QImage first;
        QImage second;
        QImage spoutFirst;
        QImage spoutSecond;
        std::uint64_t generation = 0;
        bool firstReady = false;
        bool secondReady = false;
    };

    enum class DisplayPhase
    {
        Idle,
        NoiseReduction,
        Deinterlace,
        ConvertFirst,
        SpoutFirst,
        ConvertSecond,
        SpoutSecond
    };

    //struct ReconstructedLumaFrameSlot
    //{
    //    std::atomic<std::uint64_t> generation{ 0 };
    //    std::atomic_bool writing{ false };

    //    ReconstructedLumaFrame frame;
    //};

    //struct LumaWorker
    //{
    //    LineResampler reconstructor{
    //        kLumaReconstructionRadius,
    //        kLumaReconstructionCutoff
    //    };

    //    std::vector<float> sourceLine;
    //    std::vector<float> reconstructedLine;
    //};

    WssDecoder wssDecoder_;

    std::atomic<int> wssTestFormatCode_{ -1 };
    std::atomic<int> wssTestFramesRemaining_{ 0 };

    // General state

    std::atomic<int> selectedLine_{
        kDefaultSelectedLine
    };

    std::atomic<bool> videoHighlightEnabled_{
        true
    };

    std::atomic<bool> videoLineHighlightEnabled_{
        true
    };

    std::atomic<bool> flatFieldSpectrumRequested_{
        false
    };

    std::atomic<bool> noiseReductionEnabled_{
        false
    };

    std::atomic<int> noiseReductionIntensity_{
        50
    };

    std::atomic_bool pcmDecoderEnabled_{ false };
    // 0 = Auto, 1 = Sony PCM-F1/EIAJ, 2 = Ham PCM 2.0.
    std::atomic<int> pcmDecoderMode_{ 0 };
    // Resolved format used by Auto; 0 while probing.
    std::atomic<int> pcmAutoResolvedMode_{ 0 };
    int pcmAutoSonyGoodFrames_ = 0;
    int pcmAutoHamGoodFrames_ = 0;
    int pcmAutoBadFrames_ = 0;
    std::atomic_bool pcmAudioOutputEnabled_{ false };
    std::atomic<int> pcmAudioOutputBackend_{ 0 };
    std::atomic_bool pcmMuteBottomTwoBits_{ false };
    std::atomic<int> pcmDeEmphasisMode_{ 0 }; // 0 Auto, 1 Off, 2 50/15 us

    std::atomic<bool> lumaCompensationEnabled_{
        false
    };

    std::atomic<int> lumaCompensationGainHundredthsDb_{
        60
    };

    std::atomic<bool> lumaCompensationSourceEnabled_{
        true
    };

    std::atomic<bool> spoutVideoEnabled_{
        false
    };

    std::atomic<bool> videoScreenRenderEnabled_{
        true
    };

    std::atomic<bool> waveformScreenRenderEnabled_{
        true
    };

    std::atomic<bool> vectorscopeSuppressedForAudioScope_{ false };
    std::atomic<bool> vectorscopeScreenRenderEnabled_{
        true
    };

    // Output sizes

    std::atomic<int> videoOutputWidth_{
        kDefaultVideoWidth
    };

    std::atomic<int> videoOutputHeight_{
        kDefaultVideoHeight
    };

    std::atomic<int> waveformOutputWidth_{
        kMinimumOutputSize
    };

    std::atomic<int> waveformOutputHeight_{
        kMinimumOutputSize
    };

    std::atomic<double> waveformVideoContentScale_{
        0.80
    };

    std::atomic<OpenScopeSettings::AspectRatio>
        waveformVideoAspectRatio_{
            OpenScopeSettings::AspectRatio::Ratio16x9
        };

    std::atomic<int> vectorscopeOutputWidth_{
        kMinimumOutputSize
    };

    std::atomic<int> vectorscopeOutputHeight_{
        kMinimumOutputSize
    };

    std::atomic<int> vectorscopeVideoOutputWidth_{
        kDefaultVideoWidth
    };

    std::atomic<int> vectorscopeVideoOutputHeight_{
        kDefaultVideoHeight
    };

    std::atomic<double> vectorscopeVideoContentScaleX_{ 0.80 };
    std::atomic<double> vectorscopeVideoContentScaleY_{ 0.90 };
    std::atomic_bool vectorscopeVideoEnabled_{ false };
    std::atomic_bool waveformVideoEnabled_{ false };

    std::mutex vectorscopePresentationMutex_;
    VectorscopePresentationInfo vectorscopePresentationInfo_;

    // Capture buffers

    std::array<
        CapturedFrameSlot,
        kFrameSlotCount> captureSlots_;

    std::atomic<int> latestCaptureSlot_{
        kInvalidSlotIndex
    };

    std::atomic<int> latestVectorscopeSlot_{
        kInvalidSlotIndex
    };

    std::atomic<int> latestWaveformSlot_{
        kInvalidSlotIndex
    };

    std::size_t nextCaptureWriteSlot_ = 0;
    std::size_t activeCaptureWriteSlot_ = 0;

    std::atomic<std::uint64_t> captureGeneration_{
        0
    };

    std::array<PcmCaptureSlot, kFrameSlotCount> pcmCaptureSlots_;
    std::atomic<int> latestPcmSlot_{ kInvalidSlotIndex };
    std::size_t nextPcmWriteSlot_ = 0;

    // One-shot source-frame snapshot for high-resolution PNG export.
    // submitWriteFrame() owns the source slot while copying, so the copy
    // cannot race with the DeckLink writer.
    std::mutex exportSnapshotMutex_;
    std::condition_variable exportSnapshotCondition_;
    bool exportSnapshotRequested_ = false;
    bool exportSnapshotReady_ = false;
    Yuv444Frame exportSnapshotFrame_;

    // Reconstructed luma buffers

    //std::array<
    //    ReconstructedLumaFrameSlot,
    //    kFrameSlotCount> reconstructedSlots_;

    //std::atomic<int> latestReconstructedSlot_{
    //    kInvalidSlotIndex
    //};

    std::size_t nextReconstructedWriteSlot_ = 0;

    // Display worker

    QThread displayThread_;
    std::mutex displayMutex_;
    std::condition_variable displayCondition_;

    bool displayStop_ = false;
    std::uint64_t displayLastGeneration_ = 0;

    // Waveform worker

    QThread waveformThread_;
    std::mutex waveformMutex_;
    std::condition_variable waveformCondition_;

    bool waveformStop_ = false;
    std::uint64_t waveformLastGeneration_ = 0;

    // Vectorscope worker

    QThread vectorscopeThread_;
    std::mutex vectorscopeMutex_;
    std::condition_variable vectorscopeCondition_;

    bool vectorscopeStop_ = false;
    std::uint64_t vectorscopeLastGeneration_ = 0;

    // PCM decoder worker - native 720x576 luma only.
    std::thread pcmThread_;
    std::mutex pcmMutex_;
    std::condition_variable pcmCondition_;
    bool pcmStop_ = false;
    std::uint64_t pcmLastGeneration_ = 0;
    std::atomic<bool> pcmResetRequested_{ false };
    PcmVideoDecoder pcmDecoder_;
    HamPcmV2Decoder hamPcmDecoder_;
    WasapiPcmOutput pcmAudioOutput_;
    HamWasapiPcmOutput hamPcmAudioOutput_;
    AsioPcmOutput asioPcmAudioOutput_;
    bool pcmDeEmphasisActive_ = false;
    std::array<double, 2> pcmDeEmphasisX1_{};
    std::array<double, 2> pcmDeEmphasisY1_{};

    // Luma workers

    //std::vector<std::jthread> lumaThreads_;
    //std::vector<LumaWorker> lumaWorkers_;

    //std::mutex lumaMutex_;
    //std::condition_variable lumaCondition_;
    //std::condition_variable lumaDoneCondition_;

    //bool lumaStop_ = false;

    //const Yuv444Frame* lumaFrame_ = nullptr;
    //ReconstructedLumaFrame* lumaOutputFrame_ = nullptr;

    //int lumaWorkersRemaining_ = 0;
    //std::uint64_t lumaGeneration_ = 0;

    // Luma coordinator

    //std::jthread lumaCoordinatorThread_;
    //std::mutex lumaInputMutex_;
    //std::condition_variable lumaInputCondition_;

    //bool lumaCoordinatorStop_ = false;
    //std::uint64_t lumaLastCaptureGeneration_ = 0;

    // Processing components

    std::array<
        DisplayConverter,
        2> displayConverters_;
    std::array<
        DisplayConverter,
        2> spoutVideoConverters_;
    WaveformRenderer waveformScreenRenderer_;
    WaveformRenderer waveformVideoRenderer_;
    VectorscopeRenderer vectorscopeScreenRenderer_{
        VectorscopeRenderer::Profile::Screen
    };

    VectorscopeRenderer vectorscopeVideoRenderer_{
        VectorscopeRenderer::Profile::Video
    };

    NoiseReducer noiseReducer_;
    LumaHighFrequencyCompensator lumaHighFrequencyCompensator_;
    Yuv444Frame noiseReducedFrame_;

    VideoDeinterlacer videoDeinterlacer_;
    ProgressiveLumaPair progressiveLuma_;

    PerformanceStats performanceStats_;

    LineResampler singleLineReconstructor_{
    kLumaReconstructionRadius,
    kLumaReconstructionCutoff
    };


    static constexpr std::size_t kWaveformRawCaptureFrames = 250;
    static constexpr std::size_t kWaveformRawCaptureSamples = 2880;
    std::mutex waveformRawCaptureMutex_;
    bool waveformRawCaptureActive_ = false;
    int waveformRawCaptureLine_ = -1;
    std::size_t waveformRawCaptureFrameCount_ = 0;
    std::string waveformRawCapturePath_;
    std::vector<std::uint16_t> waveformRawCaptureSamples_;
    std::vector<std::uint64_t> waveformRawCaptureGenerations_;
    std::jthread waveformRawCaptureWriter_;

    void captureWaveformRawFrame(
        const std::vector<float>& reconstructedSamples,
        std::uint64_t generation,
        int renderedLine);

    // Worker loops

    void displayWorkerLoop();
    void displayPhaseWorkerLoop(
        std::size_t workerIndex);
    void runDisplayPhase(
        DisplayPhase phase);
    void runWaveformTraceJobs(
        char phaseLabel,
        std::size_t jobCount,
        const std::function<void(
            std::size_t,
            std::uint32_t)>& job,
        TraceRendererId rendererId);
    void runFrequencyCompensationJobs(
        CapturedFrameSlot& slot,
        int gainHundredthsDb);
    [[nodiscard]] bool hasLumaCompensationConsumer() const noexcept;
    bool tryRunFrequencyAssistJob(
        std::uint32_t workerId);
    bool tryRunWaveformAssistJob(
        std::uint32_t workerId,
        bool enforceDisplayHoldoff);
    [[nodiscard]] bool canDisplayWorkerAcceptAssist(
        std::size_t workerIndex) const;
    void displayPresenterLoop();
    void waveformWorkerLoop();
    void vectorscopeWorkerLoop();
    void pcmDecoderWorkerLoop();
    void publishPcmFrame(
        const Yuv444Frame& frame,
        std::uint64_t generation);
    void lumaCoordinatorLoop();

    void lumaWorkerLoop(
        std::size_t workerIndex);

    // Slot helpers

    int findCaptureSlotByGeneration(
        std::uint64_t generation) const;

    bool isCaptureSlotValid(
        std::size_t slotIndex,
        std::uint64_t generation) const;

    bool isReconstructedSlotValid(
        std::size_t slotIndex,
        std::uint64_t generation) const;

    std::size_t acquireNextCaptureWriteSlot();
    std::size_t acquireNextReconstructedWriteSlot();

    //std::vector<float> singleLineSource_;
    //std::vector<float> singleLineReconstructed_;
    // Processing

    void reconstructLuma(
        const Yuv444Frame& frame,
        std::uint64_t generation);

    std::array<
        std::jthread,
        2> displayPhaseWorkers_;

    std::mutex displayPhaseMutex_;
    std::condition_variable displayPhaseCondition_;
    std::condition_variable displayPhaseDoneCondition_;

    bool displayPhaseStop_ = false;
    DisplayPhase displayPhase_ =
        DisplayPhase::Idle;

    std::uint64_t displayPhaseGeneration_ = 0;
    std::size_t displayPhaseWorkersRemaining_ = 0;

    const Yuv444Frame* displayPhaseFrame_ = nullptr;
    const std::uint16_t* displayPhaseLuma_ = nullptr;

    const Yuv444Frame* displayNoiseSource_ = nullptr;
    Yuv444Frame* displayNoiseDestination_ = nullptr;
    int displayNoiseIntensity_ = 0;

    QRgb* displayPhaseOutputPixels_ = nullptr;
    int displayPhaseOutputStridePixels_ = 0;
    int displayPhaseOutputWidth_ = 0;
    int displayPhaseOutputHeight_ = 0;

    std::array<
        DisplayPerformance,
        2> displayPhasePerformance_;

    static constexpr int kDeinterlaceChunkLines = 8;
    std::atomic<int> displayDeinterlaceNextLine_{ 0 };

    std::array<
        std::atomic<std::uint64_t>,
        2> displayDeinterlaceWorkerUs_{};


    // Conditional F queue.  The capture thread only orchestrates/barriers.
    // Only the two HIGH-priority display workers may consume F chunks:
    //   0 = Display worker 1
    //   1 = Display worker 2
    //
    // This is intentional: no NORMAL-priority instrument worker may become
    // part of a barrier on which the hard video path waits.
    std::mutex frequencyAssistMutex_;
    std::condition_variable frequencyAssistDoneCondition_;
    CapturedFrameSlot* frequencyAssistSlot_ = nullptr;
    int frequencyAssistGainHundredthsDb_ = 0;
    std::size_t frequencyAssistJobCount_ = 0;
    std::size_t frequencyAssistNextJob_ = 0;
    std::size_t frequencyAssistJobsRunning_ = 0;
    bool frequencyAssistActive_ = false;
    std::atomic_bool frequencyAssistWorkAvailable_{false};
    std::atomic<std::uint64_t> frequencyAssistGeneration_{0};

    // Opportunistic low-priority work for the existing display phase worker.
    // The waveform thread also consumes these jobs itself.  Display phases
    // always take priority; helper work is checked only between small stripes.
    std::mutex waveformAssistMutex_;
    std::condition_variable waveformAssistDoneCondition_;
    std::function<void(
        std::size_t,
        std::uint32_t)> waveformAssistJob_;
    std::size_t waveformAssistJobCount_ = 0;
    std::size_t waveformAssistNextJob_ = 0;
    std::size_t waveformAssistJobsRunning_ = 0;
    bool waveformAssistActive_ = false;

    // Fast wake predicate for display workers.  This remains true while the
    // current R/X assist generation still has unclaimed jobs, even if a
    // helper already observed that generation before being pre-empted by
    // display work.
    std::atomic_bool waveformAssistWorkAvailable_{false};
    char waveformAssistPhaseLabel_ = '?';
    TraceRendererId waveformAssistRendererId_ = TraceRendererId::PcWaveform;
    std::atomic<std::uint64_t> waveformAssistGeneration_{ 0 };
    std::atomic<std::uint64_t> waveformAssistCompletedGeneration_{ 0 };
    std::atomic<std::int64_t> waveformAssistCaptureTickNs_{ 0 };
    std::atomic<std::int64_t> waveformAssistTimelineOriginNs_{ 0 };
    std::atomic<std::int64_t> waveformAssistCompletedCaptureTickNs_{ 0 };

    // Display workers may consume low-priority renderer chunks only while
    // there is enough measured slack before the next 40 ms capture tick.
    // The holdoff is an EMA of the worker's actual display load + 50%.
    std::atomic<bool> displayPipelineActive_{ false };
    // At most one nice-to-have instrument worker receives the temporary
    // scheduler boost while the hard-video path is idle.
    // 0 = none, 1 = Waveform, 2 = Vectorscope.
    std::atomic<int> instrumentPriorityOwner_{ 0 };
    std::array<
        std::atomic<std::uint64_t>,
        2> displayCurrentFrameWorkerUs_{};
    std::array<
        std::atomic<std::uint64_t>,
        2> displayAssistWorkEstimateUs_{};
    std::atomic<std::int64_t> latestCaptureTickNs_{ 0 };
    // Identity of the frame currently in the display pipeline and the common
    // processing-time zero used by Field/worker diagnostic bars.
    std::atomic<std::int64_t> displayTimelineCaptureNs_{ 0 };
    std::atomic<std::int64_t> displayTimelineOriginNs_{ 0 };

    std::thread displayPresenterThread_;
    std::mutex displayPresenterMutex_;
    std::condition_variable displayPresenterCondition_;

    bool displayPresenterStop_ = false;

    std::uint64_t latestCaptureTickGeneration_ = 0;
    std::uint64_t presenterLastTickGeneration_ = 0;
    std::chrono::steady_clock::time_point latestCaptureTickTime_{};

    std::array<
        DisplayFrameSlot,
        kFrameSlotCount> displayFrameSlots_;

    QImage lastPresentedFirst_;
    QImage lastPresentedSecond_;
    QImage lastPresentedSpoutFirst_;
    QImage lastPresentedSpoutSecond_;
    bool lastPresentedPairValid_ = false;

    std::atomic<int> waveformZoomFactor_{ 1 };

    std::atomic<double> waveformScrollPosition_{ 0.0 };

    std::atomic<int> waveformPersistence_{ 0 };
    std::atomic<int> waveformCoreIntensity_{ 100 };
    std::atomic<int> waveformCoreWidthTenths_{ 10 };
    std::atomic<int> vectorscopePersistence_{ 0 };
    std::atomic<int> vectorscopeGlow_{ 50 };
};