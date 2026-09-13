#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct PerformanceMetricSnapshot
{
    std::uint64_t latestUs = 0;
    std::uint64_t averageUs = 0;
    std::uint64_t minUs = 0;
    std::uint64_t maxUs = 0;

    double latestMs() const
    {
        return latestUs / 1000.0;
    }

    double averageMs() const
    {
        return averageUs / 1000.0;
    }

    double minMs() const
    {
        return minUs / 1000.0;
    }

    double maxMs() const
    {
        return maxUs / 1000.0;
    }
};

struct PerformanceMetric
{
    std::atomic<std::uint64_t> latestUs{ 0 };
    std::atomic<std::uint64_t> averageUs{ 0 };
    std::atomic<std::uint64_t> minUs{ 0 };
    std::atomic<std::uint64_t> maxUs{ 0 };

    void update(std::uint64_t elapsedUs)
    {
        latestUs.store(
            elapsedUs,
            std::memory_order_relaxed);

        const std::uint64_t previousAverage =
            averageUs.load(
                std::memory_order_relaxed);

        if (previousAverage == 0)
        {
            averageUs.store(
                elapsedUs,
                std::memory_order_relaxed);
        }
        else
        {
            averageUs.store(
                (previousAverage * 15 + elapsedUs) / 16,
                std::memory_order_relaxed);
        }

        const std::uint64_t previousMin =
            minUs.load(
                std::memory_order_relaxed);

        if (previousMin == 0 ||
            elapsedUs < previousMin)
        {
            minUs.store(
                elapsedUs,
                std::memory_order_relaxed);
        }

        const std::uint64_t previousMax =
            maxUs.load(
                std::memory_order_relaxed);

        if (elapsedUs > previousMax)
        {
            maxUs.store(
                elapsedUs,
                std::memory_order_relaxed);
        }
    }

    PerformanceMetricSnapshot snapshot() const
    {
        return
        {
            latestUs.load(std::memory_order_relaxed),
            averageUs.load(std::memory_order_relaxed),
            minUs.load(std::memory_order_relaxed),
            maxUs.load(std::memory_order_relaxed)
        };
    }
};


struct GlowWorkloadSnapshot
{
    std::uint32_t dirtyTiles = 0;
    std::uint32_t totalTiles = 0;
    std::uint32_t horizontalPass1Tiles = 0;
    std::uint32_t verticalPass1Tiles = 0;
    std::uint32_t horizontalPass2Tiles = 0;
    std::uint32_t verticalPass2Tiles = 0;
    std::int32_t activeX = 0;
    std::int32_t activeY = 0;
    std::int32_t activeWidth = 0;
    std::int32_t activeHeight = 0;
};

struct GlowWorkloadStats
{
    std::atomic<std::uint32_t> dirtyTiles{ 0 };
    std::atomic<std::uint32_t> totalTiles{ 0 };
    std::atomic<std::uint32_t> horizontalPass1Tiles{ 0 };
    std::atomic<std::uint32_t> verticalPass1Tiles{ 0 };
    std::atomic<std::uint32_t> horizontalPass2Tiles{ 0 };
    std::atomic<std::uint32_t> verticalPass2Tiles{ 0 };
    std::atomic<std::int32_t> activeX{ 0 };
    std::atomic<std::int32_t> activeY{ 0 };
    std::atomic<std::int32_t> activeWidth{ 0 };
    std::atomic<std::int32_t> activeHeight{ 0 };

    void update(
        std::uint32_t dirty,
        std::uint32_t total,
        std::uint32_t horizontal1,
        std::uint32_t vertical1,
        std::uint32_t horizontal2,
        std::uint32_t vertical2,
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height)
    {
        dirtyTiles.store(dirty, std::memory_order_relaxed);
        totalTiles.store(total, std::memory_order_relaxed);
        horizontalPass1Tiles.store(horizontal1, std::memory_order_relaxed);
        verticalPass1Tiles.store(vertical1, std::memory_order_relaxed);
        horizontalPass2Tiles.store(horizontal2, std::memory_order_relaxed);
        verticalPass2Tiles.store(vertical2, std::memory_order_relaxed);
        activeX.store(x, std::memory_order_relaxed);
        activeY.store(y, std::memory_order_relaxed);
        activeWidth.store(width, std::memory_order_relaxed);
        activeHeight.store(height, std::memory_order_relaxed);
    }

    GlowWorkloadSnapshot snapshot() const
    {
        return
        {
            dirtyTiles.load(std::memory_order_relaxed),
            totalTiles.load(std::memory_order_relaxed),
            horizontalPass1Tiles.load(std::memory_order_relaxed),
            verticalPass1Tiles.load(std::memory_order_relaxed),
            horizontalPass2Tiles.load(std::memory_order_relaxed),
            verticalPass2Tiles.load(std::memory_order_relaxed),
            activeX.load(std::memory_order_relaxed),
            activeY.load(std::memory_order_relaxed),
            activeWidth.load(std::memory_order_relaxed),
            activeHeight.load(std::memory_order_relaxed)
        };
    }
};


struct WaveformAssistChunkEventSnapshot
{
    std::uint8_t phase = static_cast<std::uint8_t>('?');
    std::uint32_t startUs = 0;
    std::uint32_t durationUs = 0;
    std::uint32_t jobIndex = 0;
};

struct WaveformAssistTimelineSnapshot
{
    static constexpr std::size_t kCapacity = 64;
    std::uint64_t generation = 0;
    std::uint32_t count = 0;
    std::array<WaveformAssistChunkEventSnapshot, kCapacity> events{};
};

enum class FrequencyCompensationState : std::uint8_t
{
    Disabled = 0,
    NoConsumer = 1,
    Completed = 2
};

struct WaveformAssistTimelineStats
{
    static constexpr std::size_t kCapacity =
        WaveformAssistTimelineSnapshot::kCapacity;

    std::atomic<std::uint64_t> generation{ 0 };
    std::atomic<std::uint32_t> count{ 0 };
    std::array<std::atomic<std::uint8_t>, kCapacity> phase{};
    std::array<std::atomic<std::uint32_t>, kCapacity> startUs{};
    std::array<std::atomic<std::uint32_t>, kCapacity> durationUs{};
    std::array<std::atomic<std::uint32_t>, kCapacity> jobIndex{};

    void reset(std::uint64_t newGeneration = 0) noexcept
    {
        count.store(0, std::memory_order_release);
        generation.store(newGeneration, std::memory_order_release);
    }

    void append(
        char phaseLabel,
        std::uint32_t start,
        std::uint32_t duration,
        std::uint32_t chunkIndex = 0u) noexcept
    {
        const std::uint32_t index =
            count.load(std::memory_order_relaxed);

        if (index >= kCapacity)
        {
            return;
        }

        phase[index].store(
            static_cast<std::uint8_t>(phaseLabel),
            std::memory_order_relaxed);
        startUs[index].store(start, std::memory_order_relaxed);
        durationUs[index].store(duration, std::memory_order_relaxed);
        jobIndex[index].store(chunkIndex, std::memory_order_relaxed);
        count.store(index + 1u, std::memory_order_release);
    }

    [[nodiscard]] WaveformAssistTimelineSnapshot snapshot() const noexcept
    {
        WaveformAssistTimelineSnapshot result;
        const std::uint64_t beforeGeneration =
            generation.load(std::memory_order_acquire);
        result.generation = beforeGeneration;
        result.count = std::min<std::uint32_t>(
            count.load(std::memory_order_acquire),
            static_cast<std::uint32_t>(kCapacity));

        for (std::uint32_t i = 0; i < result.count; ++i)
        {
            result.events[i].phase =
                phase[i].load(std::memory_order_relaxed);
            result.events[i].startUs =
                startUs[i].load(std::memory_order_relaxed);
            result.events[i].durationUs =
                durationUs[i].load(std::memory_order_relaxed);
            result.events[i].jobIndex =
                jobIndex[i].load(std::memory_order_relaxed);
        }

        const std::uint64_t afterGeneration =
            generation.load(std::memory_order_acquire);
        if (afterGeneration != beforeGeneration)
        {
            result.generation = afterGeneration;
            result.count = 0;
        }

        return result;
    }
};


struct WaveformPhaseEventSnapshot
{
    std::uint8_t label = static_cast<std::uint8_t>('X');
    std::uint32_t startUs = 0;
    std::uint32_t durationUs = 0;
};

struct WaveformPhaseTimelineSnapshot
{
    // Screen-waveform renderer sub-phases only.  Top-level Screen/Spout/
    // measurement/publish chronology is kept separately on the worker lane.
    static constexpr std::size_t kCapacity = 64;
    std::uint64_t generation = 0;
    std::uint32_t count = 0;
    std::array<WaveformPhaseEventSnapshot, kCapacity> events{};
};

struct WaveformPhaseTimelineStats
{
    static constexpr std::size_t kCapacity =
        WaveformPhaseTimelineSnapshot::kCapacity;

    std::atomic<std::uint64_t> generation{ 0 };
    std::atomic<std::uint32_t> count{ 0 };
    std::array<std::atomic<std::uint8_t>, kCapacity> label{};
    std::array<std::atomic<std::uint32_t>, kCapacity> startUs{};
    std::array<std::atomic<std::uint32_t>, kCapacity> durationUs{};

    void reset(std::uint64_t newGeneration = 0) noexcept
    {
        count.store(0, std::memory_order_release);
        generation.store(newGeneration, std::memory_order_release);
    }

    void append(
        char phaseLabel,
        std::uint32_t start,
        std::uint32_t duration) noexcept
    {
        const std::uint32_t index =
            count.load(std::memory_order_relaxed);
        if (index >= kCapacity || duration == 0u)
        {
            return;
        }

        label[index].store(
            static_cast<std::uint8_t>(phaseLabel),
            std::memory_order_relaxed);
        startUs[index].store(start, std::memory_order_relaxed);
        durationUs[index].store(duration, std::memory_order_relaxed);
        count.store(index + 1u, std::memory_order_release);
    }

    [[nodiscard]] WaveformPhaseTimelineSnapshot snapshot() const noexcept
    {
        WaveformPhaseTimelineSnapshot result;
        const std::uint64_t beforeGeneration =
            generation.load(std::memory_order_acquire);
        result.generation = beforeGeneration;
        result.count = std::min<std::uint32_t>(
            count.load(std::memory_order_acquire),
            static_cast<std::uint32_t>(kCapacity));
        for (std::uint32_t i = 0; i < result.count; ++i)
        {
            result.events[i].label = label[i].load(std::memory_order_relaxed);
            result.events[i].startUs = startUs[i].load(std::memory_order_relaxed);
            result.events[i].durationUs = durationUs[i].load(std::memory_order_relaxed);
        }
        const std::uint64_t afterGeneration =
            generation.load(std::memory_order_acquire);
        if (afterGeneration != beforeGeneration)
        {
            result.generation = afterGeneration;
            result.count = 0;
        }
        return result;
    }
};


struct DisplayPhaseEventSnapshot
{
    std::uint8_t phase = static_cast<std::uint8_t>('?');
    std::uint32_t startUs = 0;
    std::uint32_t durationUs = 0;
};

struct DisplayPhaseTimelineSnapshot
{
    static constexpr std::size_t kCapacity = 32;
    std::uint64_t generation = 0;
    std::uint32_t count = 0;
    std::array<DisplayPhaseEventSnapshot, kCapacity> events{};
};

struct DisplayPhaseTimelineStats
{
    static constexpr std::size_t kCapacity = DisplayPhaseTimelineSnapshot::kCapacity;

    std::atomic<std::uint64_t> generation{ 0 };
    std::atomic<std::uint32_t> count{ 0 };
    std::array<std::atomic<std::uint8_t>, kCapacity> phase{};
    std::array<std::atomic<std::uint32_t>, kCapacity> startUs{};
    std::array<std::atomic<std::uint32_t>, kCapacity> durationUs{};

    void reset(std::uint64_t newGeneration = 0) noexcept
    {
        count.store(0, std::memory_order_release);
        generation.store(newGeneration, std::memory_order_release);
    }

    void append(char phaseLabel, std::uint32_t start, std::uint32_t duration) noexcept
    {
        if (duration == 0u)
        {
            return;
        }

        /*
         * Each display timeline has a single producer.  Do not publish the
         * new count until the complete slot has been written: snapshot() may
         * run concurrently from the UI thread, and publishing count first
         * allowed it to observe an uninitialised/stale slot as an Unknown
         * display phase.
         */
        const std::uint32_t index =
            count.load(std::memory_order_relaxed);
        if (index >= kCapacity)
        {
            return;
        }

        phase[index].store(
            static_cast<std::uint8_t>(phaseLabel),
            std::memory_order_relaxed);
        startUs[index].store(start, std::memory_order_relaxed);
        durationUs[index].store(duration, std::memory_order_relaxed);

        count.store(index + 1u, std::memory_order_release);
    }

    [[nodiscard]] DisplayPhaseTimelineSnapshot snapshot() const noexcept
    {
        DisplayPhaseTimelineSnapshot result;
        const std::uint64_t beforeGeneration = generation.load(std::memory_order_acquire);
        result.generation = beforeGeneration;
        result.count = std::min<std::uint32_t>(
            count.load(std::memory_order_acquire),
            static_cast<std::uint32_t>(kCapacity));

        for (std::uint32_t i = 0; i < result.count; ++i)
        {
            result.events[i].phase = phase[i].load(std::memory_order_relaxed);
            result.events[i].startUs = startUs[i].load(std::memory_order_relaxed);
            result.events[i].durationUs = durationUs[i].load(std::memory_order_acquire);
        }

        const std::uint64_t afterGeneration = generation.load(std::memory_order_acquire);
        if (afterGeneration != beforeGeneration)
        {
            result.generation = afterGeneration;
            result.count = 0;
        }
        return result;
    }
};


struct TimingDiagnosticEventSnapshot
{
    std::uint64_t count = 0;
    std::uint64_t intervalUs = 0;
    std::uint64_t value = 0;
};

struct TimingDiagnosticEventStats
{
    std::atomic<std::uint64_t> count{ 0 };
    std::atomic<std::uint64_t> lastTimestampUs{ 0 };
    std::atomic<std::uint64_t> intervalUs{ 0 };
    std::atomic<std::uint64_t> value{ 0 };

    void record(
        std::uint64_t timestampUs,
        std::uint64_t eventValue = 0) noexcept
    {
        const std::uint64_t previous =
            lastTimestampUs.exchange(
                timestampUs,
                std::memory_order_acq_rel);

        if (previous != 0u && timestampUs > previous)
        {
            intervalUs.store(
                timestampUs - previous,
                std::memory_order_relaxed);
        }

        value.store(eventValue, std::memory_order_relaxed);
        count.fetch_add(1u, std::memory_order_relaxed);
    }

    [[nodiscard]] TimingDiagnosticEventSnapshot snapshot() const noexcept
    {
        return
        {
            count.load(std::memory_order_relaxed),
            intervalUs.load(std::memory_order_relaxed),
            value.load(std::memory_order_relaxed)
        };
    }
};

using WorkerPhaseTimelineSnapshot = DisplayPhaseTimelineSnapshot;
using WorkerPhaseTimelineStats = DisplayPhaseTimelineStats;

struct PerformanceSnapshot
{
    PerformanceMetricSnapshot reconstruct;
    PerformanceMetricSnapshot noiseReduction;

    PerformanceMetricSnapshot deinterlace;
    PerformanceMetricSnapshot deinterlaceWorker0;
    PerformanceMetricSnapshot deinterlaceWorker1;

    // Per-display-worker phase timings.  These are wall-clock durations spent
    // by each of the two display workers in the strict N -> D -> C1 -> S1 -> C2 -> S2 pipeline.
    PerformanceMetricSnapshot displayWorker0Noise;
    PerformanceMetricSnapshot displayWorker0Deinterlace;
    PerformanceMetricSnapshot displayWorker0Convert1;
    PerformanceMetricSnapshot displayWorker0Spout1;
    PerformanceMetricSnapshot displayWorker0Convert2;
    PerformanceMetricSnapshot displayWorker0Spout2;

    PerformanceMetricSnapshot displayWorker1Noise;
    PerformanceMetricSnapshot displayWorker1Deinterlace;
    PerformanceMetricSnapshot displayWorker1Convert1;
    PerformanceMetricSnapshot displayWorker1Spout1;
    PerformanceMetricSnapshot displayWorker1Convert2;
    PerformanceMetricSnapshot displayWorker1Spout2;

    PerformanceMetricSnapshot videoScreen;
    PerformanceMetricSnapshot waveformScreen;
    PerformanceMetricSnapshot waveformVideo;
    PerformanceMetricSnapshot vectorscopeScreen;
    PerformanceMetricSnapshot vectorscopeVideo;

    PerformanceMetricSnapshot waveformScreenPersistence;
    PerformanceMetricSnapshot waveformScreenBaseClear;
    PerformanceMetricSnapshot waveformScreenGraticule;
    PerformanceMetricSnapshot waveformScreenPhosphorCompose;
    PerformanceMetricSnapshot waveformScreenTrace;
    PerformanceMetricSnapshot waveformScreenTracePrep;
    PerformanceMetricSnapshot waveformScreenTraceRaster;
    PerformanceMetricSnapshot waveformScreenCompose;
    PerformanceMetricSnapshot waveformScreenGlow;
    PerformanceMetricSnapshot waveformScreenOverlay;
    bool waveformScreenTraceParallel = false;
    bool waveformScreenOutputSizeChanged = false;
    bool waveformScreenOutputBufferCapacityGrew = false;
    bool waveformScreenResamplerCacheRebuilt = false;
    std::uint32_t waveformScreenTraceJobCount = 0;
    std::uint32_t waveformScreenCatWuzleChunkCount = 0;
    std::uint32_t waveformScreenCatWuzleInvalidChunkCount = 0;
    std::uint64_t waveformScreenCatWuzleZipperUs = 0;
    std::uint64_t waveformScreenCatWuzleChunkRenderMinUs = 0;
    std::uint64_t waveformScreenCatWuzleChunkRenderAvgUs = 0;
    std::uint64_t waveformScreenCatWuzleChunkRenderMaxUs = 0;
    std::uint64_t waveformScreenCatWuzleChunkQueueWaitMaxUs = 0;
    std::array<std::uint32_t, 4> waveformScreenCatWuzleWorkerChunkCount{};
    std::array<std::uint64_t, 4> waveformScreenCatWuzleWorkerRenderUs{};
    std::uint64_t waveformScreenAssistTotalUs = 0;
    std::uint64_t waveformScreenAssistFinalWaitStartUs = 0;
    std::uint64_t waveformScreenAssistFinalWaitUs = 0;
    double waveformScreenBeamCoreRadiusPx = 0.0;
    std::int32_t waveformScreenBeamCoreMarginPx = 0;
    std::uint32_t waveformScreenWidth = 0;
    std::uint32_t waveformScreenHeight = 0;

    PerformanceMetricSnapshot waveformVideoPersistence;
    PerformanceMetricSnapshot waveformVideoTrace;
    PerformanceMetricSnapshot waveformVideoCompose;
    PerformanceMetricSnapshot waveformVideoGlow;
    PerformanceMetricSnapshot waveformVideoOverlay;

    PerformanceMetricSnapshot vectorscopeScreenAnalyzer;
    PerformanceMetricSnapshot vectorscopeScreenGlowPersistence;
    PerformanceMetricSnapshot vectorscopeScreenCompose;
    PerformanceMetricSnapshot vectorscopeScreenOverlay;

    PerformanceMetricSnapshot vectorscopeVideoAnalyzer;
    PerformanceMetricSnapshot vectorscopeVideoGlowPersistence;
    PerformanceMetricSnapshot vectorscopeVideoCompose;
    PerformanceMetricSnapshot vectorscopeVideoOverlay;

    GlowWorkloadSnapshot waveformScreenGlowWorkload;
    GlowWorkloadSnapshot waveformVideoGlowWorkload;
    GlowWorkloadSnapshot vectorscopeScreenGlowWorkload;
    GlowWorkloadSnapshot vectorscopeVideoGlowWorkload;

    PerformanceMetricSnapshot displayFirst;
    PerformanceMetricSnapshot spoutConvertFirst;
    PerformanceMetricSnapshot field1Ready;
    PerformanceMetricSnapshot field1Margin;

    PerformanceMetricSnapshot displaySecond;
    PerformanceMetricSnapshot spoutConvertSecond;
    PerformanceMetricSnapshot field2Ready;
    PerformanceMetricSnapshot field2Margin;

    PerformanceMetricSnapshot presentInterval;
    PerformanceMetricSnapshot field1Present;
    PerformanceMetricSnapshot field2Present;

    PerformanceMetricSnapshot spoutQueueDelay;
    PerformanceMetricSnapshot spoutSend;
    PerformanceMetricSnapshot spoutInterval;

    std::uint64_t field1DeadlineMisses = 0;
    std::uint64_t field2DeadlineMisses = 0;

    TimingDiagnosticEventSnapshot presenterReanchor;
    TimingDiagnosticEventSnapshot presenterGenerationSkip;
    TimingDiagnosticEventSnapshot waveformGenerationSkip;
    TimingDiagnosticEventSnapshot vectorscopeGenerationSkip;

    FrequencyCompensationState frequencyCompensationState =
        FrequencyCompensationState::Disabled;
    std::uint64_t frequencyCompensationCompleteUs = 0;
    std::array<WaveformAssistTimelineSnapshot, 2> frequencyWorkerPhases{};

    PerformanceMetricSnapshot displayAllocation;
    PerformanceMetricSnapshot displaySetup;
    PerformanceMetricSnapshot displayCompose;
    PerformanceMetricSnapshot displayInterpolation;
    PerformanceMetricSnapshot displayColorConversion;
    PerformanceMetricSnapshot displayOutput;

    WorkerPhaseTimelineSnapshot waveformWorkerPhases;
    WorkerPhaseTimelineSnapshot vectorscopeWorkerPhases;

    WaveformAssistTimelineSnapshot waveformWorkerAssist;
    WaveformAssistTimelineSnapshot displayWorker0Assist;
    WaveformAssistTimelineSnapshot displayWorker1Assist;
    WaveformAssistTimelineSnapshot vectorscopeWorkerAssist;
    WaveformPhaseTimelineSnapshot waveformScreenPhases;
    WaveformPhaseTimelineSnapshot waveformVideoPhases;
    DisplayPhaseTimelineSnapshot displayFieldPhases;
    DisplayPhaseTimelineSnapshot displayWorker0Phases;
    DisplayPhaseTimelineSnapshot displayWorker1Phases;

    // Priority ownership is sampled from the central mask-transition log at
    // snapshot time.  Keep this separate from worker work timelines: ownership
    // can change after a worker has published its own completed phase snapshot.
    WorkerPhaseTimelineSnapshot priorityVideo1;
    WorkerPhaseTimelineSnapshot priorityVideo2;
    WorkerPhaseTimelineSnapshot priorityWaveform;
    WorkerPhaseTimelineSnapshot priorityVectorscope;
};

struct PerformanceStats
{
    PerformanceMetric reconstruct;
    PerformanceMetric noiseReduction;

    PerformanceMetric deinterlace;
    PerformanceMetric deinterlaceWorker0;
    PerformanceMetric deinterlaceWorker1;

    PerformanceMetric displayWorker0Noise;
    PerformanceMetric displayWorker0Deinterlace;
    PerformanceMetric displayWorker0Convert1;
    PerformanceMetric displayWorker0Spout1;
    PerformanceMetric displayWorker0Convert2;
    PerformanceMetric displayWorker0Spout2;

    PerformanceMetric displayWorker1Noise;
    PerformanceMetric displayWorker1Deinterlace;
    PerformanceMetric displayWorker1Convert1;
    PerformanceMetric displayWorker1Spout1;
    PerformanceMetric displayWorker1Convert2;
    PerformanceMetric displayWorker1Spout2;

    WorkerPhaseTimelineStats waveformWorkerPhaseTimeline;
    WorkerPhaseTimelineStats vectorscopeWorkerPhaseTimeline;

    WaveformAssistTimelineStats waveformWorkerAssistTimeline;
    std::array<WaveformAssistTimelineStats, 2> displayWorkerAssistTimeline;
    WaveformAssistTimelineStats vectorscopeWorkerAssistTimeline;
    WaveformPhaseTimelineStats waveformScreenPhaseTimeline;
    WaveformPhaseTimelineStats waveformVideoPhaseTimeline;

    // The three assist timelines and the waveform phase timeline are written
    // asynchronously while a waveform frame is being processed.  The
    // Performance floaty must never observe a half-written generation, so
    // publish one coherent completed set only after the waveform render ends.
    mutable std::mutex waveformDiagnosticPublishMutex;
    WorkerPhaseTimelineSnapshot publishedWaveformWorkerPhases;
    WaveformAssistTimelineSnapshot publishedWaveformWorkerAssist;
    WaveformAssistTimelineSnapshot publishedDisplayWorker0Assist;
    WaveformAssistTimelineSnapshot publishedDisplayWorker1Assist;
    WaveformAssistTimelineSnapshot publishedVectorscopeWorkerAssist;
    WaveformPhaseTimelineSnapshot publishedWaveformScreenPhases;
    WaveformPhaseTimelineSnapshot publishedWaveformVideoPhases;

    mutable std::mutex vectorscopeDiagnosticPublishMutex;
    WorkerPhaseTimelineSnapshot publishedVectorscopeWorkerPhases;

    mutable std::mutex frequencyDiagnosticPublishMutex;
    FrequencyCompensationState publishedFrequencyCompensationState =
        FrequencyCompensationState::Disabled;
    std::uint64_t publishedFrequencyCompensationCompleteUs = 0;
    std::array<WaveformAssistTimelineSnapshot, 2>
        publishedFrequencyWorkerPhases{};

    void publishFrequencyDiagnostics(
        FrequencyCompensationState state,
        std::uint64_t completeUs,
        const std::array<WaveformAssistTimelineSnapshot, 2>& workers) noexcept
    {
        std::lock_guard<std::mutex> lock(
            frequencyDiagnosticPublishMutex);

        publishedFrequencyCompensationState = state;
        publishedFrequencyCompensationCompleteUs = completeUs;
        publishedFrequencyWorkerPhases = workers;
    }

    DisplayPhaseTimelineStats displayFieldPhaseTimeline;
    std::array<DisplayPhaseTimelineStats, 2> displayWorkerPhaseTimeline;
    mutable std::mutex displayDiagnosticPublishMutex;
    DisplayPhaseTimelineSnapshot publishedDisplayFieldPhases;
    DisplayPhaseTimelineSnapshot publishedDisplayWorker0Phases;
    DisplayPhaseTimelineSnapshot publishedDisplayWorker1Phases;
    DisplayPhaseTimelineSnapshot previousPublishedDisplayFieldPhases;
    DisplayPhaseTimelineSnapshot previousPublishedDisplayWorker0Phases;
    DisplayPhaseTimelineSnapshot previousPublishedDisplayWorker1Phases;

    void publishDisplayDiagnosticTimelines() noexcept
    {
        const DisplayPhaseTimelineSnapshot field = displayFieldPhaseTimeline.snapshot();
        const DisplayPhaseTimelineSnapshot worker0 = displayWorkerPhaseTimeline[0].snapshot();
        const DisplayPhaseTimelineSnapshot worker1 = displayWorkerPhaseTimeline[1].snapshot();

        std::lock_guard<std::mutex> lock(displayDiagnosticPublishMutex);
        previousPublishedDisplayFieldPhases = publishedDisplayFieldPhases;
        previousPublishedDisplayWorker0Phases = publishedDisplayWorker0Phases;
        previousPublishedDisplayWorker1Phases = publishedDisplayWorker1Phases;
        publishedDisplayFieldPhases = field;
        publishedDisplayWorker0Phases = worker0;
        publishedDisplayWorker1Phases = worker1;
    }

    void publishWaveformDiagnosticTimelines() noexcept
    {
        const WorkerPhaseTimelineSnapshot workerPhases =
            waveformWorkerPhaseTimeline.snapshot();
        const WaveformAssistTimelineSnapshot waveformWorker =
            waveformWorkerAssistTimeline.snapshot();
        const WaveformAssistTimelineSnapshot displayWorker0 =
            displayWorkerAssistTimeline[0].snapshot();
        const WaveformAssistTimelineSnapshot displayWorker1 =
            displayWorkerAssistTimeline[1].snapshot();
        const WaveformAssistTimelineSnapshot vectorscopeWorker =
            vectorscopeWorkerAssistTimeline.snapshot();
        const WaveformPhaseTimelineSnapshot phases =
            waveformScreenPhaseTimeline.snapshot();
        const WaveformPhaseTimelineSnapshot videoPhases =
            waveformVideoPhaseTimeline.snapshot();

        std::lock_guard<std::mutex> lock(waveformDiagnosticPublishMutex);
        publishedWaveformWorkerPhases = workerPhases;
        publishedWaveformWorkerAssist = waveformWorker;
        publishedDisplayWorker0Assist = displayWorker0;
        publishedDisplayWorker1Assist = displayWorker1;
        publishedVectorscopeWorkerAssist = vectorscopeWorker;
        publishedWaveformScreenPhases = phases;
        publishedWaveformVideoPhases = videoPhases;
    }

    void clearPublishedWaveformDiagnosticTimelines() noexcept
    {
        std::lock_guard<std::mutex> lock(waveformDiagnosticPublishMutex);
        publishedWaveformWorkerPhases = {};
        publishedWaveformWorkerAssist = {};
        publishedDisplayWorker0Assist = {};
        publishedDisplayWorker1Assist = {};
        publishedVectorscopeWorkerAssist = {};
        publishedWaveformScreenPhases = {};
        publishedWaveformVideoPhases = {};
    }

    void clearPublishedVectorscopeDiagnosticTimeline() noexcept
    {
        std::lock_guard<std::mutex> lock(vectorscopeDiagnosticPublishMutex);
        publishedVectorscopeWorkerPhases = {};
    }

    void publishVectorscopeDiagnosticTimeline() noexcept
    {
        const WorkerPhaseTimelineSnapshot phases =
            vectorscopeWorkerPhaseTimeline.snapshot();

        std::lock_guard<std::mutex> lock(vectorscopeDiagnosticPublishMutex);
        publishedVectorscopeWorkerPhases = phases;
    }

    PerformanceMetric videoScreen;
    PerformanceMetric waveformScreen;
    PerformanceMetric waveformVideo;
    PerformanceMetric vectorscopeScreen;
    PerformanceMetric vectorscopeVideo;

    PerformanceMetric waveformScreenPersistence;
    PerformanceMetric waveformScreenBaseClear;
    PerformanceMetric waveformScreenGraticule;
    PerformanceMetric waveformScreenPhosphorCompose;
    PerformanceMetric waveformScreenTrace;
    PerformanceMetric waveformScreenTracePrep;
    PerformanceMetric waveformScreenTraceRaster;
    PerformanceMetric waveformScreenCompose;
    PerformanceMetric waveformScreenGlow;
    PerformanceMetric waveformScreenOverlay;
    std::atomic<bool> waveformScreenTraceParallel{ false };
    std::atomic<bool> waveformScreenOutputSizeChanged{ false };
    std::atomic<bool> waveformScreenOutputBufferCapacityGrew{ false };
    std::atomic<bool> waveformScreenResamplerCacheRebuilt{ false };
    std::atomic<std::uint32_t> waveformScreenTraceJobCount{ 0 };
    std::atomic<std::uint32_t> waveformScreenCatWuzleChunkCount{ 0 };
    std::atomic<std::uint32_t> waveformScreenCatWuzleInvalidChunkCount{ 0 };
    std::atomic<std::uint64_t> waveformScreenCatWuzleZipperUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenCatWuzleChunkRenderMinUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenCatWuzleChunkRenderAvgUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenCatWuzleChunkRenderMaxUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenCatWuzleChunkQueueWaitMaxUs{ 0 };
    std::array<std::atomic<std::uint32_t>, 4> waveformScreenCatWuzleWorkerChunkCount{};
    std::array<std::atomic<std::uint64_t>, 4> waveformScreenCatWuzleWorkerRenderUs{};
    std::atomic<std::uint64_t> waveformScreenAssistTotalUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenAssistFinalWaitStartUs{ 0 };
    std::atomic<std::uint64_t> waveformScreenAssistFinalWaitUs{ 0 };
    std::atomic<double> waveformScreenBeamCoreRadiusPx{ 0.0 };
    std::atomic<std::int32_t> waveformScreenBeamCoreMarginPx{ 0 };
    std::atomic<std::uint32_t> waveformScreenWidth{ 0 };
    std::atomic<std::uint32_t> waveformScreenHeight{ 0 };

    PerformanceMetric waveformVideoPersistence;
    PerformanceMetric waveformVideoTrace;
    PerformanceMetric waveformVideoCompose;
    PerformanceMetric waveformVideoGlow;
    PerformanceMetric waveformVideoOverlay;

    PerformanceMetric vectorscopeScreenAnalyzer;
    PerformanceMetric vectorscopeScreenGlowPersistence;
    PerformanceMetric vectorscopeScreenCompose;
    PerformanceMetric vectorscopeScreenOverlay;

    PerformanceMetric vectorscopeVideoAnalyzer;
    PerformanceMetric vectorscopeVideoGlowPersistence;
    PerformanceMetric vectorscopeVideoCompose;
    PerformanceMetric vectorscopeVideoOverlay;

    GlowWorkloadStats waveformScreenGlowWorkload;
    GlowWorkloadStats waveformVideoGlowWorkload;
    GlowWorkloadStats vectorscopeScreenGlowWorkload;
    GlowWorkloadStats vectorscopeVideoGlowWorkload;

    PerformanceMetric displayFirst;
    PerformanceMetric spoutConvertFirst;
    PerformanceMetric field1Ready;
    PerformanceMetric field1Margin;

    PerformanceMetric displaySecond;
    PerformanceMetric spoutConvertSecond;
    PerformanceMetric field2Ready;
    PerformanceMetric field2Margin;

    PerformanceMetric presentInterval;
    PerformanceMetric field1Present;
    PerformanceMetric field2Present;

    PerformanceMetric spoutQueueDelay;
    PerformanceMetric spoutSend;
    PerformanceMetric spoutInterval;

    std::atomic<std::uint64_t> field1DeadlineMisses{ 0 };
    std::atomic<std::uint64_t> field2DeadlineMisses{ 0 };

    TimingDiagnosticEventStats presenterReanchor;
    TimingDiagnosticEventStats presenterGenerationSkip;
    TimingDiagnosticEventStats waveformGenerationSkip;
    TimingDiagnosticEventStats vectorscopeGenerationSkip;

    PerformanceMetric displayAllocation;
    PerformanceMetric displaySetup;
    PerformanceMetric displayCompose;
    PerformanceMetric displayInterpolation;
    PerformanceMetric displayColorConversion;
    PerformanceMetric displayOutput;

    PerformanceSnapshot snapshot() const
    {
        WorkerPhaseTimelineSnapshot publishedWaveformWorkerPhaseSnapshot;
        WorkerPhaseTimelineSnapshot publishedVectorscopeWorkerPhaseSnapshot;
        WaveformAssistTimelineSnapshot publishedWaveformWorker;
        WaveformAssistTimelineSnapshot publishedDisplayWorker0;
        WaveformAssistTimelineSnapshot publishedDisplayWorker1;
        WaveformAssistTimelineSnapshot publishedVectorscopeWorkerAssistSnapshot;
        WaveformPhaseTimelineSnapshot publishedWaveformPhases;
        WaveformPhaseTimelineSnapshot publishedWaveformVideoPhaseSnapshot;
        DisplayPhaseTimelineSnapshot publishedDisplayField;
        DisplayPhaseTimelineSnapshot publishedDisplayWorker0Phase;
        DisplayPhaseTimelineSnapshot publishedDisplayWorker1Phase;
        DisplayPhaseTimelineSnapshot previousDisplayField;
        DisplayPhaseTimelineSnapshot previousDisplayWorker0Phase;
        DisplayPhaseTimelineSnapshot previousDisplayWorker1Phase;
        FrequencyCompensationState publishedFrequencyState =
            FrequencyCompensationState::Disabled;
        std::uint64_t publishedFrequencyCompleteUs = 0;
        std::array<WaveformAssistTimelineSnapshot, 2>
            publishedFrequencyWorkers{};
        {
            std::lock_guard<std::mutex> lock(waveformDiagnosticPublishMutex);
            publishedWaveformWorkerPhaseSnapshot = publishedWaveformWorkerPhases;
            publishedWaveformWorker = publishedWaveformWorkerAssist;
            publishedDisplayWorker0 = publishedDisplayWorker0Assist;
            publishedDisplayWorker1 = publishedDisplayWorker1Assist;
            publishedVectorscopeWorkerAssistSnapshot = publishedVectorscopeWorkerAssist;
            publishedWaveformPhases = publishedWaveformScreenPhases;
            publishedWaveformVideoPhaseSnapshot = publishedWaveformVideoPhases;
        }
        {
            std::lock_guard<std::mutex> lock(vectorscopeDiagnosticPublishMutex);
            publishedVectorscopeWorkerPhaseSnapshot =
                publishedVectorscopeWorkerPhases;
        }
        {
            std::lock_guard<std::mutex> lock(
                frequencyDiagnosticPublishMutex);
            publishedFrequencyState =
                publishedFrequencyCompensationState;
            publishedFrequencyCompleteUs =
                publishedFrequencyCompensationCompleteUs;
            publishedFrequencyWorkers =
                publishedFrequencyWorkerPhases;
        }
        {
            std::lock_guard<std::mutex> lock(displayDiagnosticPublishMutex);
            publishedDisplayField = publishedDisplayFieldPhases;
            publishedDisplayWorker0Phase = publishedDisplayWorker0Phases;
            publishedDisplayWorker1Phase = publishedDisplayWorker1Phases;
            previousDisplayField = previousPublishedDisplayFieldPhases;
            previousDisplayWorker0Phase = previousPublishedDisplayWorker0Phases;
            previousDisplayWorker1Phase = previousPublishedDisplayWorker1Phases;
        }

        // Prefer a display snapshot with the exact same capture identity as
        // the published waveform diagnostic frame.  Display can advance one
        // frame ahead while waveform is still rendering; keeping one previous
        // completed display snapshot lets the floaty remain frame-coherent
        // instead of combining unrelated generations.
        const std::uint64_t waveformGeneration =
            publishedWaveformPhases.generation;
        if (waveformGeneration != 0 &&
            publishedDisplayField.generation != waveformGeneration &&
            previousDisplayField.generation == waveformGeneration)
        {
            publishedDisplayField = previousDisplayField;
            publishedDisplayWorker0Phase = previousDisplayWorker0Phase;
            publishedDisplayWorker1Phase = previousDisplayWorker1Phase;
        }

        return
        {
            reconstruct.snapshot(),
            noiseReduction.snapshot(),

            deinterlace.snapshot(),
            deinterlaceWorker0.snapshot(),
            deinterlaceWorker1.snapshot(),

            displayWorker0Noise.snapshot(),
            displayWorker0Deinterlace.snapshot(),
            displayWorker0Convert1.snapshot(),
            displayWorker0Spout1.snapshot(),
            displayWorker0Convert2.snapshot(),
            displayWorker0Spout2.snapshot(),

            displayWorker1Noise.snapshot(),
            displayWorker1Deinterlace.snapshot(),
            displayWorker1Convert1.snapshot(),
            displayWorker1Spout1.snapshot(),
            displayWorker1Convert2.snapshot(),
            displayWorker1Spout2.snapshot(),

            videoScreen.snapshot(),
            waveformScreen.snapshot(),
            waveformVideo.snapshot(),
            vectorscopeScreen.snapshot(),
            vectorscopeVideo.snapshot(),

            waveformScreenPersistence.snapshot(),
            waveformScreenBaseClear.snapshot(),
            waveformScreenGraticule.snapshot(),
            waveformScreenPhosphorCompose.snapshot(),
            waveformScreenTrace.snapshot(),
            waveformScreenTracePrep.snapshot(),
            waveformScreenTraceRaster.snapshot(),
            waveformScreenCompose.snapshot(),
            waveformScreenGlow.snapshot(),
            waveformScreenOverlay.snapshot(),
            waveformScreenTraceParallel.load(
                std::memory_order_relaxed),
            waveformScreenOutputSizeChanged.load(
                std::memory_order_relaxed),
            waveformScreenOutputBufferCapacityGrew.load(
                std::memory_order_relaxed),
            waveformScreenResamplerCacheRebuilt.load(
                std::memory_order_relaxed),
            waveformScreenTraceJobCount.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleChunkCount.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleInvalidChunkCount.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleZipperUs.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleChunkRenderMinUs.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleChunkRenderAvgUs.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleChunkRenderMaxUs.load(
                std::memory_order_relaxed),
            waveformScreenCatWuzleChunkQueueWaitMaxUs.load(
                std::memory_order_relaxed),
            {
                waveformScreenCatWuzleWorkerChunkCount[0].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerChunkCount[1].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerChunkCount[2].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerChunkCount[3].load(std::memory_order_relaxed)
            },
            {
                waveformScreenCatWuzleWorkerRenderUs[0].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerRenderUs[1].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerRenderUs[2].load(std::memory_order_relaxed),
                waveformScreenCatWuzleWorkerRenderUs[3].load(std::memory_order_relaxed)
            },
            waveformScreenAssistTotalUs.load(std::memory_order_relaxed),
            waveformScreenAssistFinalWaitStartUs.load(std::memory_order_relaxed),
            waveformScreenAssistFinalWaitUs.load(std::memory_order_relaxed),
            waveformScreenBeamCoreRadiusPx.load(
                std::memory_order_relaxed),
            waveformScreenBeamCoreMarginPx.load(
                std::memory_order_relaxed),
            waveformScreenWidth.load(
                std::memory_order_relaxed),
            waveformScreenHeight.load(
                std::memory_order_relaxed),

            waveformVideoPersistence.snapshot(),
            waveformVideoTrace.snapshot(),
            waveformVideoCompose.snapshot(),
            waveformVideoGlow.snapshot(),
            waveformVideoOverlay.snapshot(),

            vectorscopeScreenAnalyzer.snapshot(),
            vectorscopeScreenGlowPersistence.snapshot(),
            vectorscopeScreenCompose.snapshot(),
            vectorscopeScreenOverlay.snapshot(),

            vectorscopeVideoAnalyzer.snapshot(),
            vectorscopeVideoGlowPersistence.snapshot(),
            vectorscopeVideoCompose.snapshot(),
            vectorscopeVideoOverlay.snapshot(),

            waveformScreenGlowWorkload.snapshot(),
            waveformVideoGlowWorkload.snapshot(),
            vectorscopeScreenGlowWorkload.snapshot(),
            vectorscopeVideoGlowWorkload.snapshot(),

            displayFirst.snapshot(),
            spoutConvertFirst.snapshot(),
            field1Ready.snapshot(),
            field1Margin.snapshot(),

            displaySecond.snapshot(),
            spoutConvertSecond.snapshot(),
            field2Ready.snapshot(),
            field2Margin.snapshot(),

            presentInterval.snapshot(),
            field1Present.snapshot(),
            field2Present.snapshot(),

            spoutQueueDelay.snapshot(),
            spoutSend.snapshot(),
            spoutInterval.snapshot(),

            field1DeadlineMisses.load(
                std::memory_order_relaxed),
            field2DeadlineMisses.load(
                std::memory_order_relaxed),

            presenterReanchor.snapshot(),
            presenterGenerationSkip.snapshot(),
            waveformGenerationSkip.snapshot(),
            vectorscopeGenerationSkip.snapshot(),

            publishedFrequencyState,
            publishedFrequencyCompleteUs,
            publishedFrequencyWorkers,

            displayAllocation.snapshot(),
            displaySetup.snapshot(),
            displayCompose.snapshot(),
            displayInterpolation.snapshot(),
            displayColorConversion.snapshot(),
            displayOutput.snapshot(),

            publishedWaveformWorkerPhaseSnapshot,
            publishedVectorscopeWorkerPhaseSnapshot,

            publishedWaveformWorker,
            publishedDisplayWorker0,
            publishedDisplayWorker1,
            publishedVectorscopeWorkerAssistSnapshot,
            publishedWaveformPhases,
            publishedWaveformVideoPhaseSnapshot,
            publishedDisplayField,
            publishedDisplayWorker0Phase,
            publishedDisplayWorker1Phase
        };
    }
};