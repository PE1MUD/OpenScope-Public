#pragma once

namespace OpenScopeBuild
{
#if defined(NDEBUG)
inline constexpr bool kDebugBuild = false;
#else
inline constexpr bool kDebugBuild = true;
#endif

// Trace logging is a development diagnostic only.
// Release builds must not create log.txt.
inline constexpr bool kTraceLoggingEnabled = kDebugBuild;
}
