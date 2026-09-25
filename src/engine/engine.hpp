#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "core/flat_hash.hpp"
#include "core/histogram.hpp"
#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/events.hpp"
#include "engine/paper.hpp"
#include "engine/scorer.hpp"
#include "engine/signals.hpp"
#include "market/board.hpp"
#include "ref/filings.hpp"
#include "ref/symbols.hpp"

namespace stok {

struct EngineConfig {
  // ---- universe ----
  double min_price = 0.50;
  double max_price = 10.0;
  double max_market_cap = 500e6;
  bool listed_only = true;           // exchange-listed only (no OTC)
  bool allow_unknown_price = true;   // keep tickers with no price yet (no market data)
  // ---- tiers ----
  int watch_score = 55;
  int high_score = 75;
  double alert_move_pct = 8.0;
  double alert_rvol = 5.0;
  double alert_dollar_volume = 300e3;
  double alert_max_spread_pct = 3.0;  // 0 = off; ignored while no valid quote
  double high_max_shares = 30e6;
  bool high_require_above_vwap = true;
  int watch_window_s = 90 * 60;
  int cooldown_s = 30 * 60;          // per ticker, same tier
  bool push_watch = false;           // also push WATCH to the alert channel
  // ---- scanner for movers with no news ----
  bool movers_enabled = true;
  double mover_move_pct = 25.0;
  double mover_rvol = 8.0;
  double mover_dollar_volume = 1e6;
  int mover_scan_ms = 1000;
  // ---- halts: 0 none, 1 watchlist only, 2 whole universe ----
  int halt_alerts = 1;
  // ---- timing ----
  uint64_t eval_interval_ns = 2'000'000;  // watchlist evaluation cadence
  bool busy_poll = false;
  bool sim_time = false;             // backtests: signal timestamps follow the simulated clock
  int stats_interval_s = 60;
  DilutionPolicy dilution;
  std::vector<int> outcome_horizons_s = {60, 300, 900, 1800, 3600};
  PaperConfig paper;
};

// Fraction of a typical day's volume traded by `minute_of_day` (ET),
// extended hours included. Used to turn ADV into "expected volume so far".
double expected_volume_fraction(int minute_of_day);

// The decision core. Single-threaded by design (its own pinned thread): no
// locks, all state local. Consumes news and market events from lock-free
// rings, reads prices through the seqlocked MarketBoard, and emits
// signals/journal records into other rings.
class Engine {
 public:
  Engine(EngineConfig cfg, const SymbolTable& symbols, const MarketBoard* board, const Scorer& scorer,
         FilingsHistory& filings, SpscQueue<Signal>* alerts, Waker* alert_waker, SpscQueue<JournalRecord>* journal,
         Waker* journal_waker, std::vector<std::string> source_names);

  void run(SpscQueue<NewsEvent>& news, SpscQueue<MarketEvent>& market, Waker& self_waker,
           const std::atomic<bool>& stop);

  // Individually callable for tests and replays. now_wall = epoch ns.
  void on_news(const NewsEvent& ev, uint64_t now_wall);
  void on_market_event(const MarketEvent& ev, uint64_t now_wall);
  void evaluate(uint64_t now_wall);
  void scan_movers(uint64_t now_wall);

  bool in_universe(uint32_t sym, double price, double* mcap_out = nullptr) const;
  // Closes paper positions at current marks (called when the engine stops).
  void shutdown(uint64_t now_wall);
  const PaperTrader* paper() const { return paper_.get(); }
  std::size_t watch_count() const;
  // Nothing to evaluate: no watched stories and no open paper positions.
  bool idle() const { return active_.empty() && (!paper_ || paper_->open_positions() == 0); }
  std::string stats_report(bool reset);

  struct SourceStats {
    uint64_t items = 0, first = 0, dup = 0, scored = 0, in_universe = 0, watch = 0;
    LatencyHistogram queue_ns;     // parsed -> engine
    LatencyHistogram publish_lag;  // source timestamp -> received (feed staleness)
    LatencyHistogram dup_lag;      // behind the first source, when not first
  };
  const SourceStats& source_stats(std::size_t i) const { return src_stats_[i]; }
  uint64_t signals_emitted(Tier t) const { return tier_counts_[static_cast<int>(t)]; }

 private:
  struct Watch {
    bool active = false;
    Tier tier = Tier::None;
    uint32_t sym = 0;
    uint16_t source = 0;
    int32_t score = 0;
    Catalyst catalyst = Catalyst::None;
    uint32_t score_flags = 0;
    uint32_t dilution_flags = 0;
    uint64_t news_id = 0;
    uint64_t t_news = 0;          // wall ns, news received
    int64_t published_ns = 0;
    int32_t ref_px = 0;
    uint64_t vol_at_news = 0;
    uint64_t notional_at_news = 0;
    double max_move = 0, min_move = 0;
    double amount_usd = 0, materiality = 0, market_cap = 0;
    uint32_t horizons_done = 0;
    uint64_t last_emit_ns[8] = {};
    FixedStr<192> why;
    FixedStr<256> title;
    FixedStr<320> link;
  };

  void handle_halt(uint32_t sym, bool halted, const char* reason, int64_t ts, bool from_itch, uint64_t now_wall,
                   const char* title);
  void emit(const Watch& w, Tier tier, uint64_t now_wall, const char* halt_reason = nullptr);
  void journal_outcome(const Watch& w, uint32_t horizon_s, uint64_t now_wall, const MarketHot& h, double move);
  void journal_trade(const PaperTrade& t);
  void build_why(Watch& w, const ScoreResult& r, double mcap);
  Watch* find_watch(uint32_t sym);
  // True (and records the time) unless this ticker already produced this
  // tier within the cooldown window.
  bool cooled_down(uint32_t sym, Tier t, uint64_t now_wall);
  // Takes a free slot (evicting the oldest story if all are in use), resets
  // it, and indexes it by ticker.
  Watch* activate_watch(uint32_t sym);
  void deactivate_watch(std::size_t active_pos);
  int32_t today_days(uint64_t now_wall);
  double rvol_of(uint32_t sym, uint64_t day_volume, uint64_t now_wall) const;
  uint32_t soft_penalty_flags() const;

  EngineConfig cfg_;
  const SymbolTable& symbols_;
  const MarketBoard* board_;
  const Scorer& scorer_;
  FilingsHistory& filings_;
  SpscQueue<Signal>* alerts_;
  Waker* alert_waker_;
  SpscQueue<JournalRecord>* journal_;
  Waker* journal_waker_;
  std::vector<std::string> source_names_;

  std::vector<Watch> watch_;
  std::vector<uint16_t> active_;           // slots in use (evaluate() walks only these)
  std::vector<uint16_t> free_;             // unused slots
  FlatMap64<uint16_t> watch_by_sym_{256};  // ticker -> slot
  std::unique_ptr<PaperTrader> paper_;
  struct FirstSeen {
    uint16_t source;
    uint64_t recv_ns;
  };
  FlatMap64<FirstSeen> first_seen_{8192};
  FlatMap64<uint64_t> last_news_by_sym_{1024};   // sym -> wall ns of latest scored news
  FlatMap64<uint64_t> mover_alerted_{256};       // sym -> day number
  FlatMap64<uint64_t> last_tier_ns_{256};        // (sym << 3 | tier) -> wall ns of last emit
  struct HaltSeen {
    char state;
    uint64_t t;
  };
  FlatMap64<HaltSeen> halt_seen_{256};
  std::vector<uint8_t> universe_static_;         // listing/type filter, computed once
  std::vector<SourceStats> src_stats_;
  LatencyHistogram recv_to_watch_ns_;            // response received -> WATCH emitted (processing)
  LatencyHistogram e2e_ns_;                      // news received -> ALERT/HIGH (includes market reaction)
  LatencyHistogram handle_ns_;                   // engine processing per news item
  uint64_t tier_counts_[8] = {};
  uint64_t journal_drops_ = 0, alert_drops_ = 0;
  int32_t cached_day_ = 0;
  uint64_t cached_day_until_ = 0;
};

}  // namespace stok
