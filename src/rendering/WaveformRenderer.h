#pragma once

#include <atomic>
#include "analysis/Analyzer.h"
#include "video/ReconstructedLumaFrame.h"
#include "rendering/WaveformGraticule.h"
#include "processing/SignalReconstructor.h"
#include "settings/OpenScopeSettings.h"
#include "diagnostics/TraceLogger.h"

#include <QImage>
#include <QRectF>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class QPainter;


// Minimum horizontal pixels per cycle for a visually pleasing waveform.
inline constexpr double kPixelsPerCycleForTraceBW = 6.0;

struct WaveformSettings
{
    int fillDensity = 0;
    bool color = false;
};

struct WaveformRenderPhaseEvent
{
    char label = 'X';
    std::uint64_t startUs = 0;
    std::uint64_t durationUs = 0;
};

struct WaveformRenderTimings
{
    static constexpr std::size_t kPhaseCapacity = 20;

    std::uint64_t persistenceUs = 0;
    std::uint64_t baseClearUs = 0;
    std::uint64_t graticuleUs = 0;
    std::uint64_t phosphorComposeUs = 0;
    std::uint64_t traceUs = 0;
    std::uint64_t tracePrepUs = 0;
    std::uint64_t traceRasterUs = 0;
    std::uint64_t composeUs = 0;
    std::uint64_t glowUs = 0;
    std::uint64_t overlayUs = 0;
    bool traceParallel = false;
    bool outputSizeChanged = false;
    bool outputBufferCapacityGrew = false;
    bool resamplerCacheRebuilt = false;
    std::uint32_t traceJobCount = 0;
    std::uint32_t catWuzleChunkCount = 0;
    std::uint32_t catWuzleInvalidChunkCount = 0;
    std::uint64_t catWuzleZipperUs = 0;
    std::uint64_t catWuzleChunkRenderMinUs = 0;
    std::uint64_t catWuzleChunkRenderAvgUs = 0;
    std::uint64_t catWuzleChunkRenderMaxUs = 0;
    std::uint64_t catWuzleChunkQueueWaitMaxUs = 0;
    std::array<std::uint32_t, 4> catWuzleWorkerChunkCount{};
    std::array<std::uint64_t, 4> catWuzleWorkerRenderUs{};
    std::uint32_t phaseCount = 0;
    std::array<WaveformRenderPhaseEvent, kPhaseCapacity> phases{};
    double beamCoreRadiusPx = 0.0;
    std::int32_t beamCoreMarginPx = 0;
    std::uint32_t glowDirtyTiles = 0;
    std::uint32_t glowTotalTiles = 0;
    std::uint32_t glowHorizontalPass1Tiles = 0;
    std::uint32_t glowVerticalPass1Tiles = 0;
    std::uint32_t glowHorizontalPass2Tiles = 0;
    std::uint32_t glowVerticalPass2Tiles = 0;
    std::int32_t glowActiveX = 0;
    std::int32_t glowActiveY = 0;
    std::int32_t glowActiveWidth = 0;
    std::int32_t glowActiveHeight = 0;
};

class WaveformRenderer final : public Analyzer
{
public:
    WaveformRenderer();

    void analyze(
        const Yuv444Frame& frame) override;

    void setSelectedLine(int line);
    void setPersistence(int persistence);
    void setCoreIntensity(int intensity);
    void setCoreWidth(int widthTenths);
    void setAntiAliasing(bool enabled) noexcept;
    void setColorizeIllegalLuminance(bool enabled) noexcept;
    void setGlow(int glow);
    void setOutputSize(
        int width,
        int height);
    void setTraceRendererId(TraceRendererId rendererId) noexcept;

    void setZoomed(bool zoomed);
    void setZoomFactor(int factor);
    void setScrollPosition(double position);
    void setContentScale(double scale);
    void setContentScale(
        double horizontalScale,
        double verticalScale);

    void setFitAspectRatio(bool enabled);

    [[nodiscard]] double traceBandwidthMHz() const;
    [[nodiscard]] const QImage& image() const;
    [[nodiscard]] const std::vector<float>& visibleLumaVolts() const noexcept;
    [[nodiscard]] const std::vector<float>& fullLumaVolts() const noexcept;
    [[nodiscard]] const std::vector<float>& reconstructedLumaSamples() const noexcept;
    void setPreparedReconstructedLuma(
        const std::vector<float>* samples) noexcept;
    [[nodiscard]] const WaveformRenderTimings& renderTimings() const noexcept;

    void setChromaFillIntensity(
        int intensity);

    void setColor(bool enabled);
    void setMeasurementProbePresentation(
        bool enabled,
        double normalizedX,
        double volts);

    using TraceJob = std::function<void(
        std::size_t,
        std::uint32_t)>;

    using TraceJobExecutor = std::function<void(
        char,
        std::size_t,
        const TraceJob&)>;

    using TraceHelperAvailability = std::function<bool()>;

    void setTraceJobExecutor(TraceJobExecutor executor);
    void setTraceHelperAvailability(TraceHelperAvailability availability);
    void setLineInfoOverlayEnabled(bool enabled, bool palOutput = false);

    void setAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

private:
    struct BeamPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct TracePixel
    {
        std::uint16_t red = 0;
        std::uint16_t green = 0;
        std::uint16_t blue = 0;
    };

    struct DenseSteepStats
    {
        std::uint64_t neighbourProbes = 0;
        std::uint32_t runCount = 0;
        std::uint32_t sustainedRunCount = 0;
        std::uint32_t denseRunCount = 0;
        std::uint32_t acceptedPacketCount = 0;
        std::uint32_t whiteSegmentCount = 0;
    };

    struct CatWuzleFrameStats
    {
        std::uint32_t chunkCount = 0;
        std::uint32_t invalidChunkCount = 0;
        std::uint64_t zipperUs = 0;
        std::uint64_t chunkRenderMinUs = 0;
        std::uint64_t chunkRenderAvgUs = 0;
        std::uint64_t chunkRenderMaxUs = 0;
        std::uint64_t chunkQueueWaitMaxUs = 0;
        std::array<std::uint32_t, 4> workerChunkCount{};
        std::array<std::uint64_t, 4> workerRenderUs{};
    };

    [[nodiscard]] std::vector<BeamPoint> buildCurrentLumaPolyline(
        const QRectF& scope,
        std::size_t viewOffset,
        std::size_t viewWidth) const;

    [[nodiscard]] std::vector<bool> buildDenseSteepPacketMask(
        const std::vector<BeamPoint>& polyline,
        DenseSteepStats* stats = nullptr) const;

    void renderCurrentPhosphorEnergy(
        const std::vector<BeamPoint>& polyline,
        const std::vector<bool>& denseSteepSegment,
        const QRectF& plotRect,
        std::uint64_t& glowUs,
        std::uint64_t& coreUs,
        int& activeMinX,
        int& activeMinY,
        int& activeMaxX,
        int& activeMaxY,
        CatWuzleFrameStats& frameStats,
        std::uint64_t timelineBaseUs);

    void clearScopephorFrames();
    void applyScopephorFeedback(
        std::vector<std::uint16_t>& currentEnergy,
        int& activeMinX,
        int& activeMinY,
        int& activeMaxX,
        int& activeMaxY);

    void clearOrFadeTrace();
    void clearTrace();
    void renderSingleLine(
        const Yuv444Frame& frame);

    void renderAllLines(
        const Yuv444Frame& frame);

    void composeTraceImage();
    void drawLineInfoOverlay(QPainter& painter);
    void plotLuminanceTraceRange(
        int firstPixelX,
        int lastPixelX);

    void plotBeam(
        double x,
        double y,
        int intensity,
        int red,
        int green,
        int blue,
        int clipFirstX = 0,
        int clipLastX = -1);

    void plotSegment(
        double x0,
        double y0,
        double x1,
        double y1,
        int intensity,
        int red,
        int green,
        int blue,
        int clipFirstX = 0,
        int clipLastX = -1);

    void addChromaFillPixel(
        int x,
        int y,
        int red,
        int green,
        int blue,
        int intensity);

    LineResampler singleLineReconstructor_{
    kLumaReconstructionRadius,
    kLumaReconstructionCutoff
    };

    [[nodiscard]] WaveformGraticuleLayout graticuleLayout() const;
    QRectF scaledScopeRect() const;
    void prepareImageForRender();

    QImage image_;
    QImage imageSpare1_;
    QImage imageSpare2_;

    WaveformGraticule graticule_;
    QRectF viewportRect() const;

    //    LineResampler singleLineReconstructor_;

    std::vector<std::uint32_t> hits_;
    std::vector<float> allLinesPersistence_;
    std::vector<TracePixel> trace_;
    std::vector<TracePixel> chromaTrace_;

    std::vector<float> sourceY_;
    std::vector<float> sourceU_;
    std::vector<float> sourceV_;
    std::vector<float> displayY_;
    std::vector<float> displayYMin_;
    std::vector<float> displayYMax_;
    std::vector<float> displayU_;
    std::vector<float> displayV_;

    std::vector<float> singleLineSource_;
    std::vector<float> singleLineReconstructed_;
    const std::vector<float>* preparedReconstructedLuma_ = nullptr;
    std::vector<float> fullLumaVolts_;

    // Persistent target-resolution scratch buffer. Reallocated only when the
    // waveform render target pixel count changes; cleared for each render.
    std::vector<std::uint16_t> currentPhosphorEnergy_;
    int currentPhosphorEnergyWidth_ = 0;
    int currentPhosphorEnergyHeight_ = 0;

    std::vector<std::uint16_t> scopephorPreviousEnergy_;
    int scopephorPreviousMinX_ = 0;
    int scopephorPreviousMinY_ = 0;
    int scopephorPreviousMaxX_ = -1;
    int scopephorPreviousMaxY_ = -1;

    std::array<std::uint8_t, 65536> displayLut_{};

    static constexpr int kLumaReconstructionRadius = 24;
    static constexpr float kLumaReconstructionCutoff = 1.00f;

    int selectedLine_ = -1;
    int persistence_ = 0;
    int coreIntensity_ = 200;
    int coreWidthTenths_ = 10;
    int glow_ = 10;
    std::atomic_bool antiAliasing_{true};
    std::atomic_bool colorizeIllegalLuminance_{true};
    double beamCoreRadiusPx_ = 0.82;
    std::uint64_t catWuzleGeneration_ = 0;
    TraceRendererId traceRendererId_ = TraceRendererId::None;
    std::uint64_t traceLogGeneration_ = 0;

    int zoomFactor_ = 1;
    double scrollPosition_ = 0.0;
    double contentScaleX_ = 1.0;
    double contentScaleY_ = 1.0;
    bool fitAspectRatio_ = true;
    bool lineInfoOverlayEnabled_ = false;
    bool lineInfoOverlayPalOutput_ = false;
    std::atomic_bool measurementProbePresentation_{false};
    std::atomic<double> measurementProbeNormalizedX_{0.0};
    std::atomic<double> measurementProbeVolts_{0.0};
    int chromaFillIntensity_ = 64;

    TraceJobExecutor traceJobExecutor_;
    TraceHelperAvailability traceHelperAvailability_;

    double inputSampleClockHz_ = 13'500'000.0;
    int inputSampleWidth_ = 720;

    WaveformSettings settings_;
    WaveformRenderTimings renderTimings_;
    bool outputSizeChangedSinceRender_ = false;
    bool outputBufferCapacityGrewSinceRender_ = false;

    OpenScopeSettings::AspectRatio aspectRatio_ =
        OpenScopeSettings::AspectRatio::Ratio16x9;
};