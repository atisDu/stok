#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

#include "core/common.hpp"

namespace stok {

// Bounded lock-free single-producer/single-consumer ring.
//
// - Producer and consumer indices live on separate cache lines, and each side
//   keeps a cached copy of the other's index, so the common case touches no
//   shared cache line at all.
// - Zero-copy API: the producer writes straight into the slot it claims, and
//   the consumer reads the slot in place before popping it.
// - Capacity is rounded up to a power of two, so wrap-around is a mask.
template <typename T>
class SpscQueue {
  static_assert(std::is_default_constructible_v<T>);

 public:
  explicit SpscQueue(std::size_t capacity)
      : cap_(next_pow2(capacity < 2 ? 2 : capacity)), mask_(cap_ - 1), slots_(new T[cap_]()) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // ---- producer side ----
  // Returns a slot to fill, or nullptr if the ring is full. Must be followed by
  // publish() before the next try_claim().
  T* try_claim() noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (STOK_UNLIKELY(t - cached_head_ >= cap_)) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t - cached_head_ >= cap_) return nullptr;
    }
    return &slots_[t & mask_];
  }

  void publish() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  bool try_push(const T& v) noexcept(std::is_nothrow_copy_assignable_v<T>) {
    T* s = try_claim();
    if (!s) return false;
    *s = v;
    publish();
    return true;
  }

  // ---- consumer side ----
  T* front() noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return nullptr;
    }
    return &slots_[h & mask_];
  }

  void pop() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

  std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

  std::size_t capacity() const noexcept { return cap_; }

 private:
  const std::size_t cap_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by consumer
  std::size_t cached_tail_ = 0;                          // consumer-private
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by producer
  std::size_t cached_head_ = 0;                          // producer-private
  char pad_[kCacheLine - sizeof(std::size_t)]{};
};

}  // namespace stok
