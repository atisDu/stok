#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/seqlock.hpp"

namespace stok {

inline constexpr uint64_t kNsPerMinute = 60'000'000'000ull;
inline constexpr uint64_t kRthOpenNs = (9ull * 60 + 30) * kNsPerMinute;  // 09:30 ET
inline constexpr uint64_t kRthCloseNs = 16ull * 60 * kNsPerMinute;      // 16:00 ET
inline constexpr int kBarRing = 64;                                     // minutes of history per symbol

// Fields the engine reads on every evaluation. They sit at the front of
// MarketState so they can be snapshotted without copying the minute ring.
struct MarketHot {
  int32_t last_px = 0;       // last trade, 1e-4 $
  int32_t open_px = 0;       // opening cross, else first regular-hours trade
  int32_t high_px = 0;       // day high (incl. extended hours)
  int32_t low_px = 0;
  int32_t rth_last_px = 0;   // last regular-hours trade (close proxy)
  int32_t close_px = 0;      // official closing cross, if printed
  int32_t pm_high_px = 0;    // pre-market high
  uint32_t pad0 = 0;
  uint64_t day_volume = 0;
  uint64_t notional_e4 = 0;  // sum(price_e4 * shares): VWAP = notional / volume
  uint64_t trades = 0;
  int64_t last_trade_ns = 0; // epoch ns
  char trading_state = 0;    // ITCH: T trading, H halted, P paused, Q quotation only
  char halt_reason[5] = {};
  bool reg_sho_restricted = false;
  uint8_t pad1 = 0;

  double last() const { return last_px / 1e4; }
  double vwap() const { return day_volume ? static_cast<double>(notional_e4) / 1e4 / static_cast<double>(day_volume) : 0.0; }
  bool halted() const { return trading_state == 'H' || trading_state == 'P' || trading_state == 'Q'; }
};

struct MinuteBar {
  int32_t minute = -1;  // minute of day (ET) this slot holds
  int32_t open_px = 0;
  int32_t high_px = 0;
  int32_t low_px = 0;
  int32_t close_px = 0;
  uint32_t pad = 0;
  uint64_t volume = 0;
};

struct MarketState {
  MarketHot hot;
  MinuteBar bars[kBarRing];  // ring indexed by minute & (kBarRing-1)

  // Volume traded in minutes [from_minute, now] (only minutes still in the ring).
  uint64_t volume_since_minute(int from_minute) const;
  // Close of the latest bar at or before `minute` (0 if not in the ring).
  int32_t price_at_minute(int minute) const;
};

// Per-symbol market state shared between the market-data thread (the only
// writer) and the engine (reader) via one seqlock per symbol: no locks, no
// queues, and the engine always sees a consistent snapshot. Each symbol's
// state starts on its own cache line.
class MarketBoard {
 public:
  explicit MarketBoard(std::size_t n_symbols);

  std::size_t size() const { return states_.size(); }

  // Trading date whose midnight (ET) anchors ITCH timestamps.
  void set_session_date(int y, unsigned m, unsigned d);
  int64_t midnight_ns() const { return midnight_ns_; }

  // ---- writer: market-data thread only ----
  void on_trade(uint32_t sym, int32_t px, uint64_t shares, uint64_t ns_since_midnight);
  void on_cross(uint32_t sym, int32_t px, uint64_t shares, uint64_t ns_since_midnight, char cross_type);
  void on_trading_action(uint32_t sym, char state, const char* reason4);
  void on_reg_sho(uint32_t sym, char action);
  const MarketState& writer_view(uint32_t sym) const { return states_[sym].writer_view(); }

  // ---- readers ----
  MarketHot hot(uint32_t sym) const {
    MarketHot h;
    states_[sym].load_prefix(&h, sizeof(MarketHot));
    return h;
  }
  MarketState full(uint32_t sym) const { return states_[sym].load(); }

 private:
  void apply_trade(MarketState& s, int32_t px, uint64_t shares, uint64_t ns);

  std::vector<SeqLocked<MarketState>> states_;
  int64_t midnight_ns_ = 0;
};

}  // namespace stok
