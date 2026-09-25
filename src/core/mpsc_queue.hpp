#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

#include "core/common.hpp"

namespace stok {

// Bounded lock-free multi-producer/single-consumer queue (Vyukov's bounded
// MPMC algorithm, used with one consumer). Each slot carries its own sequence
// number, so producers only contend on one fetch-add-like CAS. Used for
// logging, where any thread may produce.
template <typename T>
class MpscQueue {
 public:
  struct Cell {
    std::atomic<std::size_t> seq;
    T data;
  };

  explicit MpscQueue(std::size_t capacity)
      : cap_(next_pow2(capacity < 2 ? 2 : capacity)), mask_(cap_ - 1), cells_(new Cell[cap_]) {
    for (std::size_t i = 0; i < cap_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  // Claims a slot. Returns nullptr when full. Call commit(slot) after writing.
  T* try_claim(Cell** cell_out) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Cell* c = &cells_[pos & mask_];
      const std::size_t seq = c->seq.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          *cell_out = c;
          return &c->data;
        }
      } else if (diff < 0) {
        return nullptr;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  // At claim time seq == pos; publishing sets seq = pos + 1.
  static void commit(Cell* c) noexcept {
    c->seq.store(c->seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  template <typename F>
  bool try_push_with(F&& fill) noexcept {
    Cell* c = nullptr;
    T* slot = try_claim(&c);
    if (!slot) return false;
    fill(*slot);
    commit(c);
    return true;
  }

  // Single consumer.
  T* front() noexcept {
    Cell* c = &cells_[dequeue_pos_ & mask_];
    const std::size_t seq = c->seq.load(std::memory_order_acquire);
    if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeue_pos_ + 1) < 0) return nullptr;
    return &c->data;
  }

  void pop() noexcept {
    Cell* c = &cells_[dequeue_pos_ & mask_];
    c->seq.store(dequeue_pos_ + cap_, std::memory_order_release);
    ++dequeue_pos_;
  }

 private:
  const std::size_t cap_;
  const std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::size_t dequeue_pos_ = 0;
};

}  // namespace stok
