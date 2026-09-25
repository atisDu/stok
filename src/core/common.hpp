#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#define STOK_LIKELY(x) __builtin_expect(!!(x), 1)
#define STOK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define STOK_ALWAYS_INLINE inline __attribute__((always_inline))
#define STOK_NOINLINE __attribute__((noinline))
#define STOK_COLD __attribute__((cold, noinline))

namespace stok {

inline constexpr std::size_t kCacheLine = 64;

// Spin-wait hint: lowers power and avoids memory-order pipeline flushes while
// busy-polling.
STOK_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

inline constexpr std::size_t next_pow2(std::size_t v) noexcept {
  std::size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

}  // namespace stok
