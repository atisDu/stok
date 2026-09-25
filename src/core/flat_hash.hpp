#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "core/hash.hpp"

namespace stok {

// Open-addressing hash map with 64-bit keys, linear probing and backward-shift
// deletion (no tombstones, so probe lengths stay short after heavy churn such
// as the ITCH order book: add, execute, delete millions of times a day).
//
// One slot = key + value, contiguous, so a lookup is usually one cache miss.
// Key ~0ull is reserved as the empty marker.
template <typename V>
class FlatMap64 {
 public:
  static constexpr uint64_t kEmpty = ~0ull;

  struct Slot {
    uint64_t key;
    V value;
  };

  explicit FlatMap64(std::size_t initial_capacity = 16, double max_load = 0.7)
      : max_load_(max_load) {
    rehash(next_pow2(initial_capacity < 8 ? 8 : initial_capacity));
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return slots_.size(); }
  bool empty() const noexcept { return size_ == 0; }

  V* find(uint64_t key) noexcept {
    std::size_t i = index(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) return &s.value;
      if (s.key == kEmpty) return nullptr;
      i = (i + 1) & mask_;
    }
  }
  const V* find(uint64_t key) const noexcept { return const_cast<FlatMap64*>(this)->find(key); }

  bool contains(uint64_t key) const noexcept { return find(key) != nullptr; }

  // Returns {value*, inserted}. On insert the value is default constructed.
  std::pair<V*, bool> try_emplace(uint64_t key) {
    if (STOK_UNLIKELY(size_ + 1 > grow_at_)) rehash(slots_.size() * 2);
    std::size_t i = index(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) return {&s.value, false};
      if (s.key == kEmpty) {
        s.key = key;
        s.value = V{};
        ++size_;
        return {&s.value, true};
      }
      i = (i + 1) & mask_;
    }
  }

  V* insert_or_assign(uint64_t key, const V& v) {
    auto [p, inserted] = try_emplace(key);
    *p = v;
    return p;
  }

  bool erase(uint64_t key) noexcept {
    std::size_t i = index(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == kEmpty) return false;
      if (s.key == key) break;
      i = (i + 1) & mask_;
    }
    // Backward-shift: pull later entries of the same cluster into the hole if
    // their ideal slot is at or before it.
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      Slot& sj = slots_[j];
      if (sj.key == kEmpty) break;
      const std::size_t ideal = index(sj.key);
      // Is `ideal` cyclically outside (hole, j]? Then sj may move into hole.
      const bool movable = (hole <= j) ? (ideal <= hole || ideal > j) : (ideal <= hole && ideal > j);
      if (movable) {
        slots_[hole] = sj;
        hole = j;
      }
    }
    slots_[hole].key = kEmpty;
    --size_;
    return true;
  }

  void clear() noexcept {
    for (auto& s : slots_) s.key = kEmpty;
    size_ = 0;
  }

  void reserve(std::size_t n) {
    std::size_t want = next_pow2(static_cast<std::size_t>(static_cast<double>(n) / max_load_) + 1);
    if (want > slots_.size()) rehash(want);
  }

  template <typename F>
  void for_each(F&& f) {
    for (auto& s : slots_)
      if (s.key != kEmpty) f(s.key, s.value);
  }
  template <typename F>
  void for_each(F&& f) const {
    for (const auto& s : slots_)
      if (s.key != kEmpty) f(s.key, s.value);
  }

 private:
  STOK_ALWAYS_INLINE std::size_t index(uint64_t key) const noexcept { return mix64(key) & mask_; }

  STOK_COLD void rehash(std::size_t new_cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(new_cap, Slot{kEmpty, V{}});
    mask_ = new_cap - 1;
    grow_at_ = static_cast<std::size_t>(static_cast<double>(new_cap) * max_load_);
    size_ = 0;
    for (auto& s : old) {
      if (s.key == kEmpty) continue;
      std::size_t i = index(s.key);
      while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
      slots_[i] = s;
      ++size_;
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  std::size_t grow_at_ = 0;
  double max_load_;
};

struct Unit {};

class FlatSet64 {
 public:
  explicit FlatSet64(std::size_t cap = 16) : m_(cap) {}
  // Returns true if newly inserted.
  bool insert(uint64_t k) { return m_.try_emplace(k).second; }
  bool contains(uint64_t k) const noexcept { return m_.contains(k); }
  bool erase(uint64_t k) noexcept { return m_.erase(k); }
  std::size_t size() const noexcept { return m_.size(); }
  void clear() noexcept { m_.clear(); }

 private:
  FlatMap64<Unit> m_;
};

}  // namespace stok
