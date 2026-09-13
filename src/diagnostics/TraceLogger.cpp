#include "diagnostics/TraceLogger.h"
#include "BuildConfig.h"

#include <fstream>
#include <iomanip>
#include <string_view>

namespace
{
const char* rendererName(TraceRendererId rendererId) noexcept
{
    switch (rendererId)
    {
    case TraceRendererId::None: return "-";
    case TraceRendererId::PcWaveform: return "PC";
    case TraceRendererId::SpoutWaveform: return "SPOUT";
    }
    return "?";
}

const char* eventName(TraceEventType type) noexcept
{
    switch (type)
    {
    case TraceEventType::Frame: return "FRAME";
    case TraceEventType::FocusGain: return "FOCUS_GAIN";
    case TraceEventType::FocusLost: return "FOCUS_LOST";
    case TraceEventType::PresenterField1Tick: return "TICK_F1";
    case TraceEventType::PresenterField2Tick: return "TICK_F2";
    case TraceEventType::WaveformWorkerBegin: return "WF_WORKER_BEGIN";
    case TraceEventType::WaveformWorkerEnd: return "WF_WORKER_END";
    case TraceEventType::CatGenerationBegin: return "CAT_BEGIN";
    case TraceEventType::DirectBegin: return "DIRECT_BEGIN";
    case TraceEventType::DirectCore: return "DIRECT_CORE";
    case TraceEventType::DirectCoreSegment: return "DIRECT_CORE_SEG";
    case TraceEventType::DirectCoreJoin: return "DIRECT_CORE_JOIN";
    case TraceEventType::DirectCoreGeometry: return "DIRECT_CORE_GEOM";
    case TraceEventType::DirectCoreCoverage: return "DIRECT_CORE_COVERAGE";
    case TraceEventType::DirectCoreResult: return "DIRECT_CORE_RESULT";
    case TraceEventType::DirectGlow: return "DIRECT_GLOW";
    case TraceEventType::DirectGlowWork: return "DIRECT_GLOW_WORK";
    case TraceEventType::DirectCompose: return "DIRECT_COMPOSE";
    case TraceEventType::DirectEnd: return "DIRECT_END";
    case TraceEventType::WaveformBegin: return "WF_BEGIN";
    case TraceEventType::MudDetect: return "MUD_DETECT";
    case TraceEventType::WaveformRaster: return "WF_RASTER";
    case TraceEventType::WaveformResolve: return "WF_RESOLVE";
    case TraceEventType::WaveformPersistence: return "WF_PERSIST";
    case TraceEventType::WaveformCompose: return "WF_COMPOSE";
    case TraceEventType::WaveformEnd: return "WF_END";
    case TraceEventType::AssistBegin: return "ASSIST_BEGIN";
    case TraceEventType::AssistJobStart: return "ASSIST_JOB_START";
    case TraceEventType::AssistJobEnd: return "ASSIST_JOB_END";
    case TraceEventType::AssistWaitBegin: return "ASSIST_WAIT_BEGIN";
    case TraceEventType::AssistWaitEnd: return "ASSIST_WAIT_END";
    case TraceEventType::ChunkClaim: return "CHUNK_CLAIM";
    case TraceEventType::ChunkReturn: return "CHUNK_RETURN";
    case TraceEventType::ZipperBegin: return "ZIP_BEGIN";
    case TraceEventType::ZipperChunkBegin: return "ZIP_CHUNK_BEGIN";
    case TraceEventType::ZipperChunkEnd: return "ZIP_CHUNK_END";
    case TraceEventType::ZipperEnd: return "ZIP_END";
    case TraceEventType::TraceStoppedFull: return "TRACE_STOP_FULL";
    case TraceEventType::TraceStoppedTimeout: return "TRACE_STOP_TIMEOUT";
    }
    return "UNKNOWN";
}
}

TraceLogger& TraceLogger::instance()
{
    static TraceLogger logger;
    return logger;
}

TraceLogger::TraceLogger()
    : start_(std::chrono::steady_clock::now())
{
    if constexpr (OpenScopeBuild::kTraceLoggingEnabled)
    {
        writer_ = std::thread([this]() { writerLoop(); });
    }
    else
    {
        accepting_.store(false, std::memory_order_relaxed);
    }
}

TraceLogger::~TraceLogger()
{
    accepting_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable())
    {
        writer_.join();
    }
}

void TraceLogger::stopBecauseFull() noexcept
{
    accepting_.store(false, std::memory_order_release);
    fullStopRecorded_.store(true, std::memory_order_release);
}

void TraceLogger::log(
    TraceEventType type,
    std::uint64_t generation,
    std::uint32_t workerId,
    std::uint32_t itemId,
    std::uint64_t value0,
    std::uint64_t value1,
    TraceRendererId rendererId) noexcept
{
    if constexpr (!OpenScopeBuild::kTraceLoggingEnabled)
    {
        return;
    }

    if (!accepting_.load(std::memory_order_acquire))
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t timestampUs =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - start_).count());

    if (timestampUs >= kMaxTraceUs)
    {
        accepting_.store(false, std::memory_order_release);
        return;
    }

    std::uint64_t ticket = 0;
    for (;;)
    {
        const std::uint64_t read =
            readIndex_.load(std::memory_order_acquire);
        std::uint64_t write =
            writeIndex_.load(std::memory_order_relaxed);

        if (write - read >= kCapacity)
        {
            stopBecauseFull();
            return;
        }

        if (writeIndex_.compare_exchange_weak(
                write,
                write + 1u,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            ticket = write;
            break;
        }
    }

    Slot& slot = slots_[static_cast<std::size_t>(ticket % kCapacity)];

    // A slot can only be reused after the single writer cleared ready.
    // If the writer is unexpectedly behind, stop tracing rather than ever
    // blocking a realtime render/capture thread.
    if (slot.ready.load(std::memory_order_acquire))
    {
        stopBecauseFull();
        return;
    }

    slot.event = Event{
        timestampUs,
        generation,
        value0,
        value1,
        workerId,
        itemId,
        type,
        rendererId
    };

    slot.ready.store(true, std::memory_order_release);
}

void TraceLogger::writerLoop()
{
    std::ofstream out("log.txt", std::ios::out | std::ios::trunc);
    if (!out)
    {
        accepting_.store(false, std::memory_order_release);
        return;
    }

    out << "# OpenScope trace v2\n";
    out << "# us event renderer gen worker item value0 value1\n";

    bool timeoutLineWritten = false;
    bool fullLineWritten = false;

    for (;;)
    {
        bool wroteAny = false;

        const auto writerNow = std::chrono::steady_clock::now();
        const std::uint64_t writerElapsedUs =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    writerNow - start_).count());
        if (writerElapsedUs >= kMaxTraceUs)
        {
            accepting_.store(false, std::memory_order_release);
        }

        for (;;)
        {
            const std::uint64_t read =
                readIndex_.load(std::memory_order_relaxed);
            const std::uint64_t written =
                writeIndex_.load(std::memory_order_acquire);

            if (read >= written)
            {
                break;
            }

            Slot& slot = slots_[static_cast<std::size_t>(read % kCapacity)];
            if (!slot.ready.load(std::memory_order_acquire))
            {
                break;
            }

            const Event event = slot.event;
            out << event.timestampUs << ' '
                << eventName(event.type) << ' '
                << rendererName(event.rendererId) << ' '
                << event.generation << ' '
                << event.workerId << ' '
                << event.itemId << ' '
                << event.value0 << ' '
                << event.value1 << '\n';

            slot.ready.store(false, std::memory_order_release);
            readIndex_.store(read + 1u, std::memory_order_release);
            wroteAny = true;
        }

        if (!accepting_.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedUs =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - start_).count());

            if (fullStopRecorded_.load(std::memory_order_acquire) &&
                !fullLineWritten)
            {
                out << elapsedUs << " TRACE_STOP_FULL 0 0 0 0 0\n";
                fullLineWritten = true;
            }
            else if (elapsedUs >= kMaxTraceUs && !timeoutLineWritten)
            {
                out << elapsedUs << " TRACE_STOP_TIMEOUT 0 0 0 0 0\n";
                timeoutLineWritten = true;
            }
        }

        const bool drained =
            readIndex_.load(std::memory_order_acquire) >=
            writeIndex_.load(std::memory_order_acquire);

        if (stop_.load(std::memory_order_acquire) && drained)
        {
            break;
        }

        if (!accepting_.load(std::memory_order_acquire) && drained)
        {
            // The trace is deliberately finite. Keep the writer thread cheap
            // after the queue has stopped accepting new events.
            out.flush();
            if (!stop_.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }
        else if (!wroteAny)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    out.flush();
}
