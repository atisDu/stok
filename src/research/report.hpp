#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stok::research {

// Rows loaded from the JSONL journal (only the fields the report uses).
struct NewsRow {
  std::string day, src, first_src, catalyst, kind;
  int score = 0;
  bool has_sym = false, in_universe = false, dup = false;
  double behind_ms = 0, pub_to_recv_ms = -1;
};

struct SignalRow {
  std::string day, tier, catalyst;
  int score = 0;
  double news_to_signal_ms = -1;
};

struct OutcomeRow {
  std::string day, tier, catalyst;
  int score = 0;
  int horizon_s = 0;
  double move = 0, mfe = 0, mae = 0;
};

struct TradeRow {
  std::string day, ticker, tier, catalyst, exit_reason;
  int score = 0;
  uint64_t exit_ns = 0;
  double pnl_usd = 0, pnl_pct = 0, mfe = 0, mae = 0, hold_s = 0;
};

struct JournalData {
  std::vector<std::string> days;
  std::vector<NewsRow> news;
  std::vector<SignalRow> signals;
  std::vector<OutcomeRow> outcomes;
  std::vector<TradeRow> trades;  // closed trades only
  std::size_t bad_lines = 0;
};

// Loads <dir>/YYYY-MM-DD/*.jsonl for days in [from, to] (empty = unbounded).
JournalData load_journal(const std::string& dir, const std::string& from = "", const std::string& to = "");

struct TradeStats {
  std::size_t n = 0, wins = 0, losses = 0;
  double win_rate = 0, gross_win = 0, gross_loss = 0, total = 0;
  double avg_win = 0, avg_loss = 0, expectancy = 0, expectancy_pct = 0;
  double profit_factor = 0, max_drawdown = 0, total_ex_top3 = 0;
  int span_days = 0;
};

// Trades in chronological order of exit.
TradeStats trade_stats(std::vector<TradeRow> trades);

struct GoNoGo {
  int min_trades = 150;
  int min_span_days = 56;  // 8 weeks
  double min_profit_factor = 1.3;
};

std::string render_report(const JournalData& d, const GoNoGo& g = {});

}  // namespace stok::research
