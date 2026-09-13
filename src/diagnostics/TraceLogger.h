#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

enum class TraceRendererId : std::uint8_t
{
    None = 0,
    PcWaveform = 1,
    SpoutWaveform = 2
};

enum class TraceEventType : std::uint16_t
{
    Frame,
    FocusGain,
    FocusLost,
    PresenterField1Tick,
    PresenterField2Tick,
    WaveformWorkerBegin,
    WaveformWorkerEnd,
    CatGenerationBegin,
    DirectBegin,
    DirectCore,
    DirectCoreSegment,
    DirectCoreJoin,
    DirectCoreGeometry,
    DirectCoreCoverage,
    DirectCoreResult,
    DirectGlow,
    DirectGlowWork,
    DirectCompose,
    DirectEnd,
    WaveformBegin,
    MudDetect,
    WaveformRaster,
    WaveformResolve,
    WaveformPersistence,
    WaveformCompose,
    WaveformEnd,
    AssistBegin,
    AssistJobStart,
    AssistJobEnd,
    AssistWaitBegin,
    AssistWaitEnd,
    ChunkClaim,
    ChunkReturn,
    ZipperBegin,
    ZipperChunkBegin,
    ZipperChunkEnd,
    ZipperEnd,
    TraceStoppedFull,
    TraceStoppedTimeout
};

class TraceLogger final
{
public:
    static TraceLogger& instance();

    void log(
        TraceEventType type,
        std::uint64_t generation = 0,
        std::uint32_t workerId = 0,
        std::uint32_t itemId = 0,
        std::uint64_t value0 = 0,
        std::uint64_t value1 = 0,
        TraceRendererId rendererId = TraceRendererId::None) noexcept;

    TraceLogger(const TraceLogger&) = delete;
    TraceLogger& operator=(const TraceLogger&) = delete;

private:
    TraceLogger();
    ~TraceLogger();

    struct Event
    {
        std::uint64_t timestampUs = 0;
        std::uint64_t generation = 0;
        std::uint64_t value0 = 0;
        std::uint64_t value1 = 0;
        std::uint32_t workerId = 0;
        std::uint32_t itemId = 0;
        TraceEventType type = TraceEventType::Frame;
        TraceRendererId rendererId = TraceRendererId::None;
    };

    struct Slot
    {
        std::atomic<bool> ready{ false };
        Event event;
    };

    static constexpr std::size_t kCapacity = 65536;
    static constexpr std::uint64_t kMaxTraceUs = 10'000'000ull;

    void writerLoop();
    void stopBecauseFull() noexcept;

    std::array<Slot, kCapacity> slots_{};
    std::atomic<std::uint64_t> writeIndex_{ 0 };
    std::atomic<std::uint64_t> readIndex_{ 0 };
    std::atomic<bool> accepting_{ true };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> fullStopRecorded_{ false };
    std::chrono::steady_clock::time_point start_;
    std::thread writer_;
};
