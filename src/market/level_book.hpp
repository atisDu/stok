#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/common.hpp"

namespace stok {

// One side of a price-level book, aggregated by price.
//
// Levels are kept in a contiguous vector ordered worst -> best, so the best
// price is at the back. Almost all activity happens at or near the touch, so
// inserts and erases move only a few elements, and the top of book is one
// load away. Positions are found by binary search.
class SideLadder {
 public:
  struct Level {
    uint32_t price;
    uint32_t pad;
    uint64_t qty;
  };

  explicit SideLadder(bool is_bid = true) : bid_(is_bid) {}

  void add(uint32_t price, uint64_t qty) {
    if (qty == 0) return;
    auto it = position(price);
    if (it != lv_.end() && it->price == price) {
      it->qty += qty;
      return;
    }
    lv_.insert(it, Level{price, 0, qty});
  }

  // Removes up to `qty` at `price`; drops the level when it empties.
  void remove(uint32_t price, uint64_t qty) {
    if (qty == 0 || lv_.empty()) return;
    // Fast path: the touch.
    if (lv_.back().price == price) {
      Level& l = lv_.back();
      if (qty >= l.qty) lv_.pop_back();
      else l.qty -= qty;
      return;
    }
    auto it = position(price);
    if (it == lv_.end() || it->price != price) return;  // unknown level (partial book)
    if (qty >= it->qty) lv_.erase(it);
    else it->qty -= qty;
  }

  bool empty() const { return lv_.empty(); }
  uint32_t best_price() const { return lv_.empty() ? 0 : lv_.back().price; }
  uint64_t best_qty() const { return lv_.empty() ? 0 : lv_.back().qty; }
  std::size_t depth() const { return lv_.size(); }
  const std::vector<Level>& levels() const { return lv_; }
  void clear() { lv_.clear(); }

 private:
  // First level not worse than `price` (i.e. where `price` belongs).
  std::vector<Level>::iterator position(uint32_t price) {
    if (bid_)  // ascending prices: best (highest) bid at the back
      return std::lower_bound(lv_.begin(), lv_.end(), price,
                              [](const Level& l, uint32_t p) { return l.price < p; });
    // descending prices: best (lowest) ask at the back
    return std::lower_bound(lv_.begin(), lv_.end(), price, [](const Level& l, uint32_t p) { return l.price > p; });
  }

  std::vector<Level> lv_;
  bool bid_;
};

struct LevelBook {
  SideLadder bids{true};
  SideLadder asks{false};
  // Last top of book published to the board (to publish only on change).
  uint32_t pub_bid = 0, pub_ask = 0;
  uint64_t pub_bid_q = 0, pub_ask_q = 0;
};

}  // namespace stok
