#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "core/flat_hash.hpp"
#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/events.hpp"
#include "market/board.hpp"
#include "market/itch.hpp"
#include "market/level_book.hpp"
#include "ref/symbols.hpp"

namespace stok {

// ITCH handler that turns order-level messages into trade prints and top of
// book on the MarketBoard. It tracks resting orders (ref -> price/shares/side)
// so executions, which carry only the order reference, can be priced, and
// aggregates them into per-symbol price ladders for the best bid/ask. Also
// forwards trading actions (halts / pauses / resumes) and system events to
// the engine.
//
// The book is only complete if the feed is processed from the start of the
// day (ITCH begins before 04:00 ET). Joined mid-session, resting orders
// entered earlier are unknown until they trade or cancel.
class ItchBook {
 public:
  struct Order {
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    char side;  // 'B' or 'S'
  };
  struct Stats {
    uint64_t messages = 0, adds = 0, execs = 0, trades = 0, crosses = 0, unknown_ref = 0, halts = 0;
    uint64_t quote_updates = 0;
    uint64_t max_orders = 0;
  };

  ItchBook(const SymbolTable& symbols, MarketBoard& board, SpscQueue<MarketEvent>* events, Waker* waker,
           std::size_t order_capacity, bool track_quotes = true);

  // Maps a locate code directly (tests / non-ITCH sources).
  void map_locate(uint16_t locate, uint32_t sym) { locate_to_sym_[locate] = sym; }
  uint32_t sym_for_locate(uint16_t locate) const { return locate_to_sym_[locate]; }
  const Stats& stats() const { return st_; }
  std::size_t live_orders() const { return orders_.size(); }
  char last_system_event() const { return last_sys_; }

  // ---- itch::decode handler interface ----
  void on_system_event(uint64_t ts, char code);
  void on_stock_directory(uint16_t locate, uint64_t ts, uint64_t key, char cat, char fin, uint32_t round_lot);
  void on_trading_action(uint16_t locate, uint64_t ts, uint64_t key, char state, const char* reason);
  void on_reg_sho(uint16_t locate, uint64_t ts, char action);

  STOK_ALWAYS_INLINE void on_add(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                                 uint32_t price) {
    ++st_.messages;
    const uint32_t sym = locate_to_sym_[locate];
    if (sym == SymbolTable::kInvalid) return;
    ++st_.adds;
    *orders_.try_emplace(ref).first = Order{price, shares, locate, side};
    if (track_quotes_) {
      book_side(sym, side).add(price, shares);
      publish_top(sym, ts);
    }
  }
  STOK_ALWAYS_INLINE void on_executed(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
    ++st_.messages;
    Order* o = orders_.find(ref);
    if (!o) {
      ++st_.unknown_ref;
      return;
    }
    ++st_.execs;
    board_.on_trade(locate_to_sym_[locate], static_cast<int32_t>(o->price), shares, ts);
    reduce(o, ref, shares, ts);
  }
  STOK_ALWAYS_INLINE void on_executed_price(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                                            bool printable, uint32_t price) {
    ++st_.messages;
    Order* o = orders_.find(ref);
    if (printable) {
      ++st_.execs;
      board_.on_trade(locate_to_sym_[locate], static_cast<int32_t>(price), shares, ts);
    }
    if (o) reduce(o, ref, shares, ts);
    else ++st_.unknown_ref;
  }
  STOK_ALWAYS_INLINE void on_cancel(uint16_t, uint64_t ts, uint64_t ref, uint32_t shares) {
    ++st_.messages;
    if (Order* o = orders_.find(ref)) reduce(o, ref, shares, ts);
  }
  STOK_ALWAYS_INLINE void on_delete(uint16_t, uint64_t ts, uint64_t ref) {
    ++st_.messages;
    if (Order* o = orders_.find(ref)) reduce(o, ref, o->shares, ts);
  }
  STOK_ALWAYS_INLINE void on_replace(uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                                     uint32_t shares, uint32_t price) {
    ++st_.messages;
    Order* o = orders_.find(old_ref);
    if (!o) return;
    const char side = o->side;
    const uint16_t loc = o->locate;
    const uint32_t sym = locate_to_sym_[loc];
    if (track_quotes_ && sym != SymbolTable::kInvalid) book_side(sym, side).remove(o->price, o->shares);
    orders_.erase(old_ref);
    *orders_.try_emplace(new_ref).first = Order{price, shares, loc, side};
    if (track_quotes_ && sym != SymbolTable::kInvalid) {
      book_side(sym, side).add(price, shares);
      publish_top(sym, ts);
    }
    (void)locate;
  }
  STOK_ALWAYS_INLINE void on_trade(uint16_t locate, uint64_t ts, char, uint32_t shares, uint64_t, uint32_t price) {
    ++st_.messages;
    ++st_.trades;
    board_.on_trade(locate_to_sym_[locate], static_cast<int32_t>(price), shares, ts);
  }
  void on_cross(uint16_t locate, uint64_t ts, uint64_t shares, uint64_t, uint32_t price, char cross_type) {
    ++st_.messages;
    ++st_.crosses;
    board_.on_cross(locate_to_sym_[locate], static_cast<int32_t>(price), shares, ts, cross_type);
  }
  void on_broken(uint16_t, uint64_t, uint64_t) { ++st_.messages; }

  void update_peak() {
    if (orders_.size() > st_.max_orders) st_.max_orders = orders_.size();
  }

  // Price ladders (tests / diagnostics).
  const LevelBook* book(uint32_t sym) const { return sym < books_.size() ? &books_[sym] : nullptr; }

 private:
  STOK_ALWAYS_INLINE SideLadder& book_side(uint32_t sym, char side) {
    return side == 'B' ? books_[sym].bids : books_[sym].asks;
  }

  // Pushes the top of book to the board only when it changed.
  STOK_ALWAYS_INLINE void publish_top(uint32_t sym, uint64_t ts) {
    LevelBook& b = books_[sym];
    const uint32_t bp = b.bids.best_price(), ap = b.asks.best_price();
    const uint64_t bq = b.bids.best_qty(), aq = b.asks.best_qty();
    if (bp == b.pub_bid && ap == b.pub_ask && bq == b.pub_bid_q && aq == b.pub_ask_q) return;
    b.pub_bid = bp;
    b.pub_ask = ap;
    b.pub_bid_q = bq;
    b.pub_ask_q = aq;
    ++st_.quote_updates;
    board_.on_quote(sym, static_cast<int32_t>(bp), bq, static_cast<int32_t>(ap), aq, ts);
  }

  STOK_ALWAYS_INLINE void reduce(Order* o, uint64_t ref, uint32_t shares, uint64_t ts) {
    const uint32_t take = shares < o->shares ? shares : o->shares;
    if (track_quotes_) {
      const uint32_t sym = locate_to_sym_[o->locate];
      if (sym != SymbolTable::kInvalid) {
        book_side(sym, o->side).remove(o->price, take);
        publish_top(sym, ts);
      }
    }
    if (shares >= o->shares) orders_.erase(ref);
    else o->shares -= shares;
  }

  const SymbolTable& symbols_;
  MarketBoard& board_;
  SpscQueue<MarketEvent>* events_;
  Waker* waker_;
  std::array<uint32_t, 65536> locate_to_sym_;
  std::array<char, 65536> last_state_;
  FlatMap64<Order> orders_;
  std::vector<LevelBook> books_;
  bool track_quotes_;
  Stats st_;
  char last_sys_ = 0;
};

}  // namespace stok
