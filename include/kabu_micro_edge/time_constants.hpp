#pragma once

#include <cstdint>

namespace kabu {

/// Nanoseconds per millisecond. Use for converting ms → ns durations.
inline constexpr std::int64_t NS_PER_MS = 1'000'000LL;

/// Nanoseconds per second. Use for converting seconds → ns durations.
inline constexpr std::int64_t NS_PER_SEC = 1'000'000'000LL;

}  // namespace kabu
