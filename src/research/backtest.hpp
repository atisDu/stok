#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "engine/scorer.hpp"
#include "ref/filings.hpp"
#include "ref/symbols.hpp"

namespace stok::research {

struct BacktestOptions {
  std::string news_dir;      // journal-format dir: YYYY-MM-DD/news.jsonl (recorded live or from EDGAR history)
  std::string itch_dir;      // Nasdaq ITCH files named MMDDYYYY.NASDAQ_ITCH50[.gz]
  std::string tape_dir;      // bridge recordings named YYYY-MM-DD.tape[.gz] (used when no ITCH file)
  std::string out_dir;       // output journal (stok-report reads it) + rolling baseline
  std::string ref_dir;       // security master (SEC/Nasdaq Trader reference files)
  std::string filings_path;  // filings_history.tsv: dilution flags use only filings dated before each day
  std::string rules_file = "config/rules.tsv";
  std::vector<std::string> days;  // explicit days; else every day with news in [from, to]
  std::string from, to;
  EngineConfig engine;
  int64_t news_delay_ms = 0;          // shift every arrival (what if my source were slower/faster?)
  uint64_t eval_interval_ns = 10'000'000;
  bool movers = false;                // scan for no-news movers (needs every symbol's book: slower)
  bool rolling_baseline = true;       // prev close / ADV from previously replayed days only
  std::size_t order_capacity = 1u << 22;
};

struct DayResult {
  std::string day;
  std::string market;  // "itch", "tape" or "none"
  std::size_t news = 0, market_messages = 0, signals = 0, alerts = 0, trades_closed = 0;
  double pnl = 0;
  double seconds = 0;
  std::string note;
};

// Replays recorded news against historical market data through the same
// Engine and PaperTrader the daemon runs, on a simulated clock:
//   * news events are delivered at their recorded arrival time (+ delay),
//   * market messages drive the MarketBoard in exchange-time order,
//   * the watchlist is evaluated every eval_interval of simulated time.
// Output is an ordinary journal (news, signals, outcomes, trades), so
// stok-report works on it unchanged. Deterministic: same inputs, same output.
class Backtester {
 public:
  explicit Backtester(BacktestOptions opts);
  bool init(std::string* err);
  std::vector<DayResult> run();
  DayResult run_day(const std::string& day);
  const std::vector<std::string>& days() const { return days_; }

 private:
  BacktestOptions opts_;
  SymbolTable base_;
  Scorer scorer_;
  FilingsHistory filings_;
  std::vector<std::string> days_;
  std::vector<std::string> sources_;
};

// "2019-01-30" -> "01302019"
std::string itch_name_date(const std::string& ymd);

}  // namespace stok::research
