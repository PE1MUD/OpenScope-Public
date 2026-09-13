#pragma once

#include "BuildConfig.h"
#include "diagnostics/TraceLogger.h"

inline void traceLog(
    TraceEventType type,
    std::uint64_t generation = 0,
    std::uint32_t workerId = 0,
    std::uint32_t itemId = 0,
    std::uint64_t value0 = 0,
    std::uint64_t value1 = 0,
    TraceRendererId rendererId = TraceRendererId::None) noexcept
{
    if constexpr (OpenScopeBuild::kTraceLoggingEnabled)
    {
        TraceLogger::instance().log(
            type,
            generation,
            workerId,
            itemId,
            value0,
            value1,
            rendererId);
    }
}
