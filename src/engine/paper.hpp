#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/scorer.hpp"
#include "engine/signals.hpp"
#include "market/board.hpp"
#include "ref/symbols.hpp"

namespace stok {

struct PaperConfig {
  bool enabled = false;
  bool trade_alert = true;          // open on ALERT
  bool trade_high = true;           // open on HIGH
  int latency_ms = 250;             // signal -> order arrival (your reaction / bot latency)
  double slippage_bps = 30;         // adverse slippage added to every fill
  double position_usd = 1000;       // notional per trade
  int max_open = 3;                 // open + pending positions
  double max_daily_loss_usd = 300;  // stop opening new trades after this realized loss
  double stop_loss_pct = 8;
  double take_profit_pct = 20;
  double trail_activate_pct = 10;   // trailing stop arms after this gain...
  double trail_pct = 8;             // ...and exits this far below the high-water mark
  int max_hold_min = 30;
  int flat_by_minute = 15 * 60 + 55;  // ET minute of day: close everything
  int entry_timeout_s = 10;         // cancel if not fillable in time (halt, no price)
  double max_chase_pct = 5;         // cancel if the price ran this far past the signal price
  double commission_per_share = 0.0;
  double commission_min = 0.0;
};


// Local paper trading: turns ALERT/HIGH signals into simulated positions
// using the live MarketBoard (buy at the ask after a latency, sell at the
// bid, adverse slippage on both). Exits: stop, target, trailing stop, time
// stop, flat-by time. It can't exit while a stock is halted, so gaps through
// a stop show up as real losses. Single-threaded: runs inside the engine.
class PaperTrader {
 public:
  struct Stats {
    uint64_t signals = 0, opened = 0, closed = 0, wins = 0, losses = 0;
    uint64_t rejected_risk = 0, rejected_capacity = 0, rejected_dup = 0, cancelled_chase = 0, cancelled_timeout = 0;
    double gross_win = 0, gross_loss = 0, realized = 0, peak = 0, max_drawdown = 0, day_realized = 0;
  };

  PaperTrader(PaperConfig cfg, const SymbolTable& symbols, const MarketBoard& board);

  void on_signal(const Signal& s, uint64_t now_wall);
  // Advances fills and exits. `sink(trade)` is called when a position opens
  // (closed = false) and when it closes (closed = true).
  template <typename Sink>
  void step(uint64_t now_wall, Sink&& sink);
  // Closes everything at the current marks (shutdown / end of day).
  template <typename Sink>
  void close_all(uint64_t now_wall, const char* reason, Sink&& sink);

  const Stats& stats() const { return st_; }
  std::size_t open_positions() const;
  std::string stats_line() const;

 private:
  struct Pos {
    bool active = false;
    bool filled = false;
    uint64_t eligible_ns = 0;
    PaperTrade t{};
  };

  bool try_fill(Pos& p, uint64_t now);
  bool check_exit(Pos& p, uint64_t now, const char*& reason, double& px, bool& quote);
  void finish(Pos& p, uint64_t now, const char* reason, double px, bool quote);
  void roll_day(uint64_t now);
  double fees(double shares) const;

  PaperConfig cfg_;
  const SymbolTable& symbols_;
  const MarketBoard& board_;
  std::vector<Pos> pos_;
  Stats st_;
  int32_t day_ = 0;
  uint64_t day_checked_at_ = 0;  // the ET date is re-derived at most once a minute
};

template <typename Sink>
void PaperTrader::step(uint64_t now, Sink&& sink) {
  roll_day(now);
  for (auto& p : pos_) {
    if (!p.active) continue;
    if (!p.filled) {
      if (now < p.eligible_ns) continue;
      if (try_fill(p, now)) sink(p.t);
      continue;
    }
    const char* reason = nullptr;
    double px = 0;
    bool quote = false;
    if (check_exit(p, now, reason, px, quote)) {
      finish(p, now, reason, px, quote);
      sink(p.t);
      p.active = false;
    }
  }
}

template <typename Sink>
void PaperTrader::close_all(uint64_t now, const char* reason, Sink&& sink) {
  for (auto& p : pos_) {
    if (!p.active) continue;
    if (p.filled) {
      const MarketHot h = board_.hot(p.t.sym);
      const bool quote = h.quote_valid();
      const double mark = quote ? h.bid() : h.last();
      finish(p, now, reason, mark * (1.0 - cfg_.slippage_bps / 1e4), quote);
      sink(p.t);
    }
    p.active = false;
  }
}

}  // namespace stok
