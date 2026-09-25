#pragma once

#include <cstdint>
#include <type_traits>

#include "core/fixed_string.hpp"
#include "engine/events.hpp"
#include "engine/scorer.hpp"

namespace stok {

enum class Tier : uint8_t {
  None = 0,
  Watch = 1,   // material-looking news on an in-universe ticker; no price confirmation yet
  Alert = 2,   // news + the stock is moving on volume
  High = 3,    // alert + strong score, small share count, clean filings, holding VWAP
  Halt = 4,    // halt / pause / resume on a relevant ticker
  Mover = 5,   // big mover with no qualifying news
  Info = 6,    // e.g. offering filed on a watched ticker
};

const char* tier_name(Tier t);

// One emitted signal. Trivially copyable (travels through rings).
struct Signal {
  Tier tier;
  Catalyst catalyst;
  uint16_t source;
  uint32_t sym;
  uint32_t score_flags;
  uint32_t dilution_flags;
  int32_t score;
  uint64_t news_id;
  uint64_t t_news_recv_ns;  // wall: news response received
  uint64_t t_signal_ns;     // wall: signal emitted
  int64_t published_ns;
  double price;
  double ref_price;         // last price before the news
  double move_pct;          // since the news
  double rvol;              // day volume vs expected-by-now (0 = unknown)
  double dollar_volume;     // traded since the news
  double vwap;
  double bid;
  double ask;
  double spread_pct;        // 0 = no valid quote
  double market_cap;
  double shares_out;
  double amount_usd;
  double materiality;
  char ticker[16];
  char exchange[16];
  char halt_reason[8];
  FixedStr<192> why;        // compact explanation
  FixedStr<256> title;
  FixedStr<320> link;
};
static_assert(std::is_trivially_copyable_v<Signal>);

struct Outcome {
  uint32_t sym;
  uint32_t horizon_s;
  uint64_t news_id;
  uint64_t t_news_recv_ns;
  uint64_t t_ns;
  double ref_price;
  double price;
  double move_pct;
  double max_move_pct;      // best (MFE) seen so far, sampled at eval cadence
  double min_move_pct;      // worst (MAE)
  double volume_since;
  double dollar_volume_since;
  int32_t score;
  Tier tier_reached;
  Catalyst catalyst;
  char ticker[16];
};
static_assert(std::is_trivially_copyable_v<Outcome>);

// One simulated trade (opened or closed) from the paper trader.
struct PaperTrade {
  uint32_t sym;
  Tier tier;
  Catalyst catalyst;
  bool closed;
  bool quote_entry;          // filled against a real ask (vs last + slippage)
  bool quote_exit;
  bool size_over_touch;      // our size exceeded the displayed size at the touch
  int32_t score;
  uint64_t news_id;
  uint64_t signal_ns;
  uint64_t entry_ns;
  uint64_t exit_ns;
  double signal_px;
  double entry_px;
  double exit_px;
  double shares;
  double fees;
  double pnl_usd;
  double pnl_pct;
  double mfe_pct;            // best unrealized gain while open
  double mae_pct;            // worst unrealized loss while open
  char ticker[16];
  char exit_reason[16];      // stop, target, trail, time, flat, shutdown
};
static_assert(std::is_trivially_copyable_v<PaperTrade>);

// Everything the journal thread writes. The engine fills structs; the
// journal thread does the (comparatively slow) JSON formatting and I/O.
struct JournalRecord {
  enum class Type : uint8_t { News = 0, Signal = 1, Outcome = 2, Text = 3, Trade = 4 };
  Type type;
  // News
  NewsEvent news;
  ScoreResult score;
  uint32_t news_sym;          // primary symbol scored (kInvalid if none)
  uint32_t news_dilution;
  double news_market_cap;
  double news_price;
  bool news_in_universe;
  bool news_duplicate;        // same headline already seen from another source
  uint16_t first_source;
  int64_t first_seen_lag_ns;  // this arrival minus first arrival (duplicates)
  uint64_t engine_ns;         // wall: engine handled it
  // Signal / Outcome / Trade
  Signal signal;
  Outcome outcome;
  PaperTrade trade;
  // Text: a preformatted JSON object (stats etc.) for file `text_file`
  char text_file[16];
  FixedStr<2048> text;
};
static_assert(std::is_trivially_copyable_v<JournalRecord>);

}  // namespace stok
