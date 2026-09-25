#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/events.hpp"
#include "market/baseline.hpp"
#include "market/board.hpp"
#include "market/itch_book.hpp"
#include "market/sources.hpp"
#include "ref/symbols.hpp"

namespace stok {

struct MarketOptions {
  std::string source = "none";   // none | itch_file | moldudp64 | bridge
  std::string itch_file;         // .gz or raw ITCH 5.0 (Nasdaq binary file format)
  double replay_speed = 1.0;     // 0 = as fast as possible
  std::string session_date;      // YYYY-MM-DD; default: today (ET) or from the ITCH file name
  MoldUdp64Receiver::Options mold;
  std::string bridge_bind = "127.0.0.1";
  uint16_t bridge_port = 7777;
  std::size_t order_capacity = 1u << 22;
  std::string baseline_dir;      // where history.tsv / baseline.tsv live
  bool write_baseline = true;    // append the session at end of day
  bool busy_poll = false;
};

// Owns the market-data thread's source and applies it to the MarketBoard.
class MarketRunner {
 public:
  MarketRunner(MarketOptions opts, const SymbolTable& symbols, MarketBoard& board, SpscQueue<MarketEvent>& events,
               Waker* engine_waker);

  bool init(std::string* err);
  void run(const std::atomic<bool>& stop);
  std::string stats_report() const;
  const std::string& session_date() const { return date_; }

  // Writes the session into the baseline store (market thread / after stop).
  void write_baseline();

 private:
  static void on_bridge_state(void* ctx, uint32_t sym, char state, const char* reason, int64_t ts);

  MarketOptions opts_;
  const SymbolTable& symbols_;
  MarketBoard& board_;
  SpscQueue<MarketEvent>& events_;
  Waker* waker_;
  std::string date_;
  std::unique_ptr<ItchBook> book_;
  std::unique_ptr<ItchFileReader> file_;
  std::unique_ptr<MoldUdp64Receiver> mold_;
  std::unique_ptr<BridgeReceiver> bridge_;
  bool baseline_written_ = false;
  bool file_done_ = false;
};

// "01302019.NASDAQ_ITCH50.gz" -> "2019-01-30"
std::string itch_file_date(const std::string& path);

}  // namespace stok
