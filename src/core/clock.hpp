#pragma once

#include <ctime>
#include <cstdint>

#include "core/common.hpp"

namespace stok {

// Both clocks go through the vDSO (~20 ns, no syscall).

// Wall clock, ns since the Unix epoch. Used for anything compared across
// machines or against feed timestamps.
STOK_ALWAYS_INLINE uint64_t wall_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Monotonic clock for intervals, timeouts and scheduling.
STOK_ALWAYS_INLINE uint64_t mono_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

inline constexpr uint64_t kNsPerUs = 1'000ull;
inline constexpr uint64_t kNsPerMs = 1'000'000ull;
inline constexpr uint64_t kNsPerSec = 1'000'000'000ull;

}  // namespace stok
