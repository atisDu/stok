// stok-replay: replays a Nasdaq TotalView-ITCH 5.0 file through the same
// order book + MarketBoard the daemon uses.
//   * throughput benchmark (messages/s, ns/message)
//   * builds the volume baseline from historical sessions (--baseline)
//   * prints the session's top movers / most active names
//
// Nasdaq publishes sample ITCH files (see scripts/fetch_itch_sample.sh).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/settings.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "market/baseline.hpp"
#include "market/itch_book.hpp"
#include "market/itch_directory.hpp"
#include "market/runner.hpp"
#include "market/sources.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

}  // namespace

int main(int argc, char** argv) {
  std::string file, config_path, date;
  double speed = 0.0;
  uint64_t max_msgs = 0;
  bool baseline = false;
  int top = 20;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
    else if (a == "--speed" && i + 1 < argc) speed = std::atof(argv[++i]);
    else if (a == "--max" && i + 1 < argc) max_msgs = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--date" && i + 1 < argc) date = argv[++i];
    else if (a == "--top" && i + 1 < argc) top = std::atoi(argv[++i]);
    else if (a == "--baseline") baseline = true;
    else if (!a.empty() && a[0] != '-' && file.empty()) file = a;
    else {
      std::fprintf(stderr,
                   "usage: stok-replay FILE.NASDAQ_ITCH50[.gz] [-c config] [--speed 0] [--max N] [--date YYYY-MM-DD]\n"
                   "                   [--baseline] [--top 20]\n");
      return 2;
    }
  }
  if (file.empty()) {
    std::fprintf(stderr, "missing ITCH file\n");
    return 2;
  }
  Settings s;
  if (!config_path.empty()) {
    std::string err;
    auto cfg = Config::load_file(config_path, &err);
    if (!cfg || !load_settings(*cfg, s, &err)) {
      std::fprintf(stderr, "config: %s\n", err.c_str());
      return 1;
    }
  }
  SymbolTable symbols;
  std::string rep;
  if (!config_path.empty()) symbols.load_dir(s.ref_dir, &rep);
  {
    // Pass 1: stock directory.
    std::string err;
    const long added = add_itch_directory(file, symbols, &err);
    if (added < 0) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::printf("symbols: %zu (%ld from the ITCH stock directory)\n", symbols.size(), added);
  }
  if (date.empty()) date = itch_file_date(file);
  if (date.empty()) date = timeutil::eastern_date_str(static_cast<int64_t>(wall_ns()));
  int y;
  unsigned m, d;
  if (!timeutil::parse_ymd(date, y, m, d)) {
    std::fprintf(stderr, "bad date %s\n", date.c_str());
    return 1;
  }
  MarketBoard board(symbols.size());
  board.set_session_date(y, m, d);
  ItchBook book(symbols, board, nullptr, nullptr, s.market.order_capacity);
  ItchFileReader reader;
  std::string err;
  if (!reader.open(file, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::atomic<bool> stop{false};
  const uint64_t t0 = mono_ns();
  uint64_t done = 0;
  while (reader.run(book, stop, speed, 1 << 16)) {
    book.update_peak();
    done = reader.stats().messages;
    if (max_msgs && done >= max_msgs) break;
  }
  book.update_peak();
  const double secs = static_cast<double>(mono_ns() - t0) / 1e9;
  const auto& st = reader.stats();
  const auto& bs = book.stats();
  std::printf("session %s: %llu messages, %.1f MB in %.2fs -> %.2f M msg/s, %.1f ns/msg (incl. decompression)\n",
              date.c_str(), static_cast<unsigned long long>(st.messages), static_cast<double>(st.bytes) / 1e6, secs,
              static_cast<double>(st.messages) / secs / 1e6, secs * 1e9 / static_cast<double>(std::max<uint64_t>(1, st.messages)));
  std::printf("adds=%llu execs=%llu hidden_trades=%llu crosses=%llu halts=%llu unknown_ref=%llu peak_orders=%llu bad=%llu\n",
              (unsigned long long)bs.adds, (unsigned long long)bs.execs, (unsigned long long)bs.trades,
              (unsigned long long)bs.crosses, (unsigned long long)bs.halts, (unsigned long long)bs.unknown_ref,
              (unsigned long long)bs.max_orders, (unsigned long long)st.bad);

  struct Row {
    uint32_t sym;
    double dollar, move;
  };
  std::vector<Row> rows;
  for (uint32_t i = 0; i < board.size(); ++i) {
    const MarketHot& h = board.writer_view(i).hot;
    if (h.day_volume == 0) continue;
    const double ref = symbols[i].prev_close > 0 ? symbols[i].prev_close : (h.open_px ? h.open_px / 1e4 : 0);
    rows.push_back({i, static_cast<double>(h.notional_e4) / 1e4, ref > 0 ? (h.last() - ref) * 100 / ref : 0});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.dollar > b.dollar; });
  std::printf("\nmost active by dollar volume (Nasdaq executions only):\n%-8s %12s %10s %10s %10s %8s\n", "ticker",
              "dollar_vol", "last", "vwap", "volume", "chg%");
  for (int i = 0; i < top && i < static_cast<int>(rows.size()); ++i) {
    const MarketHot& h = board.writer_view(rows[i].sym).hot;
    std::printf("%-8s %12.0f %10.4f %10.4f %10llu %+8.1f\n", symbols[rows[i].sym].ticker.c_str(), rows[i].dollar,
                h.last(), h.vwap(), static_cast<unsigned long long>(h.day_volume), rows[i].move);
  }
  if (baseline) {
    BaselineStore store(s.baseline_dir.empty() ? std::string("data/baseline") : s.baseline_dir);
    const std::size_t n = store.append_session(date, board, symbols);
    std::string r2;
    store.rebuild(20, &r2);
    std::printf("\nbaseline: appended %zu symbols for %s; %s -> %s\n", n, date.c_str(), r2.c_str(),
                store.baseline_path().c_str());
  }
  Logger::instance().stop();
  return 0;
}
