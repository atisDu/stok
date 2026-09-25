#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/common.hpp"

namespace stok {

// Single-writer, many-reader sequence lock.
//
// The writer never blocks and never touches a shared lock word that readers
// write, so the market-data thread updates state at full speed while the engine
// takes consistent snapshots. Readers retry if they overlap a write (rare: a
// write is a few stores).
//
// T must be trivially copyable. Snapshots are taken with memcpy between fences,
// the standard seqlock pattern for x86/ARM with GCC/Clang.
template <typename T>
class alignas(kCacheLine) SeqLocked {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // ---- writer (one thread only) ----
  T& begin_write() noexcept {
    seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return data_;
  }
  void end_write() noexcept {
    std::atomic_thread_fence(std::memory_order_release);
    seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  // Writer-thread direct access (no concurrent writer exists).
  const T& writer_view() const noexcept { return data_; }

  // ---- readers ----
  T load() const noexcept {
    T out;
    load_prefix(&out, sizeof(T));
    return out;
  }

  // Copies only the first `bytes` of T. Lets hot fields at the front of a large
  // struct be read without copying the whole thing.
  void load_prefix(void* out, std::size_t bytes) const noexcept {
    for (;;) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (STOK_UNLIKELY(s1 & 1u)) {
        cpu_relax();
        continue;
      }
      std::memcpy(out, &data_, bytes);
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t s2 = seq_.load(std::memory_order_relaxed);
      if (STOK_LIKELY(s1 == s2)) return;
    }
  }

  uint32_t version() const noexcept { return seq_.load(std::memory_order_acquire); }

 private:
  std::atomic<uint32_t> seq_{0};
  T data_{};
};

}  // namespace stok
