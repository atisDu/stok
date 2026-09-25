// stok-backtest: replays recorded news against historical market data with
// the live engine + paper trader, then reports.
//
//   stok-backtest -c config/stok.conf                       # all recorded days
//   stok-backtest -c config/stok.conf --from 2026-10-01 --to 2026-10-31
//   stok-backtest -c config/stok.conf --sweep signal.watch_score=55,65,75 --split 2026-10-15
//
// Inputs: news from the journal (data/journal/<day>/news.jsonl, recorded by
// stokd or built by stok-history), market data from Nasdaq ITCH files
// (data/itch/MMDDYYYY.NASDAQ_ITCH50.gz, recorded by stokd or downloaded) or
// bridge tapes (data/tape/<day>.tape[.gz]). Output: a journal directory that
// stok-report understands.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/settings.hpp"
#include "core/ascii.hpp"
#include "core/log.hpp"
#include "research/backtest.hpp"
#include "research/report.hpp"
#include "util/file.hpp"

using namespace stok;

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: stok-backtest [-c config] [--news DIR] [--itch DIR] [--tape DIR] [--out DIR]\n"
               "                     [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--day YYYY-MM-DD ...]\n"
               "                     [--news-delay-ms N] [--eval-ms 10] [--movers] [--no-paper]\n"
               "                     [--no-rolling-baseline] [--set section.key=value ...]\n"
               "                     [--sweep section.key=v1,v2,...] [--split YYYY-MM-DD] [--verbose]\n");
}

// Refuses to clobber a directory that isn't a previous backtest output.
bool prepare_out(const std::string& dir, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string marker = fileutil::join(dir, ".stok-backtest");
  if (fs::exists(dir, ec) && !fs::is_empty(dir, ec) && !fs::exists(marker, ec)) {
    if (err) *err = dir + " exists and is not a stok-backtest output; choose another --out";
    return false;
  }
  fs::remove_all(dir, ec);
  fileutil::make_dirs(dir);
  return fileutil::write_file_atomic(marker, "stok-backtest output\n");
}

struct Run {
  std::string label;
  research::TradeStats all, in, out;
  std::size_t signals = 0, alerts = 0, days = 0;
};

research::TradeStats stats_between(const research::JournalData& d, const std::string& from, const std::string& to) {
  std::vector<research::TradeRow> t;
  for (const auto& r : d.trades)
    if ((from.empty() || r.day >= from) && (to.empty() || r.day < to)) t.push_back(r);
  return research::trade_stats(t);
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/stok.conf", news_dir, itch_dir, tape_dir, out_dir, from, to, sweep, split_day;
  std::vector<std::string> days;
  std::vector<std::pair<std::string, std::string>> overrides;
  int64_t delay_ms = 0;
  int eval_ms = 10;
  bool movers = false, paper = true, rolling = true, verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "-c" || a == "--config") config_path = next();
    else if (a == "--news") news_dir = next();
    else if (a == "--itch") itch_dir = next();
    else if (a == "--tape") tape_dir = next();
    else if (a == "--out") out_dir = next();
    else if (a == "--from") from = next();
    else if (a == "--to") to = next();
    else if (a == "--day") days.push_back(next());
    else if (a == "--news-delay-ms") delay_ms = std::atoll(next().c_str());
    else if (a == "--eval-ms") eval_ms = std::max(1, std::atoi(next().c_str()));
    else if (a == "--movers") movers = true;
    else if (a == "--no-paper") paper = false;
    else if (a == "--no-rolling-baseline") rolling = false;
    else if (a == "--verbose") verbose = true;
    else if (a == "--split") split_day = next();
    else if (a == "--sweep") sweep = next();
    else if (a == "--set") {
      const std::string kv = next();
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        usage();
        return 2;
      }
      overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else {
      usage();
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  Logger::instance().set_level(verbose ? LogLevel::Info : LogLevel::Warn);
  std::string err;
  auto cfg = Config::load_file(config_path, &err);
  if (!cfg) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  auto apply = [](Config& c, const std::string& k, const std::string& v) {
    const auto dot = k.find('.');
    if (dot == std::string::npos) return false;
    c.set(k.substr(0, dot), k.substr(dot + 1), v);
    return true;
  };
  for (const auto& [k, v] : overrides) {
    if (!apply(*cfg, k, v)) {
      std::fprintf(stderr, "--set expects section.key=value\n");
      return 2;
    }
  }
  if (paper) cfg->set("paper", "enabled", "true");

  // Sweep values (or a single run).
  std::string sweep_key;
  std::vector<std::string> values = {""};
  if (!sweep.empty()) {
    const auto eq = sweep.find('=');
    if (eq == std::string::npos || sweep.find('.') > eq) {
      std::fprintf(stderr, "--sweep expects section.key=v1,v2,...\n");
      return 2;
    }
    sweep_key = sweep.substr(0, eq);
    values.clear();
    split(std::string_view(sweep).substr(eq + 1), ',', [&](std::string_view v) {
      v = trim(v);
      if (!v.empty()) values.emplace_back(v);
    });
  }

  Settings base_settings;
  if (!load_settings(*cfg, base_settings, &err)) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  const std::string root_out = out_dir.empty() ? fileutil::join(base_settings.data_dir, "backtest") : out_dir;
  if (!sweep_key.empty() && !prepare_out(root_out, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::vector<Run> runs;
  for (const auto& value : values) {
    Config c = *cfg;
    std::string label = "run";
    if (!sweep_key.empty()) {
      apply(c, sweep_key, value);
      label = sweep_key + "=" + value;
    }
    Settings s;
    if (!load_settings(c, s, &err)) {
      std::fprintf(stderr, "config (%s): %s\n", label.c_str(), err.c_str());
      return 1;
    }
    research::BacktestOptions o;
    o.news_dir = news_dir.empty() ? s.journal_dir : news_dir;
    o.itch_dir = itch_dir.empty() ? fileutil::join(s.data_dir, "itch") : itch_dir;
    o.tape_dir = tape_dir.empty() ? fileutil::join(s.data_dir, "tape") : tape_dir;
    o.out_dir = sweep_key.empty() ? root_out : fileutil::join(root_out, label);
    o.ref_dir = s.ref_dir;
    o.filings_path = fileutil::join(s.ref_dir, "filings_history.tsv");
    o.rules_file = s.rules_file;
    o.days = days;
    o.from = from;
    o.to = to;
    o.engine = s.engine;
    o.news_delay_ms = delay_ms;
    o.eval_interval_ns = static_cast<uint64_t>(eval_ms) * 1'000'000ull;
    o.movers = movers;
    o.rolling_baseline = rolling;
    o.order_capacity = s.market.order_capacity;
    if (!prepare_out(o.out_dir, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    research::Backtester bt(o);
    if (!bt.init(&err)) {
      std::fprintf(stderr, "backtest: %s\n", err.c_str());
      return 1;
    }
    std::printf("== %s: %zu day(s), news from %s -> %s\n", label.c_str(), bt.days().size(), o.news_dir.c_str(),
                o.out_dir.c_str());
    Run run;
    run.label = label;
    for (const auto& day : bt.days()) {
      const auto r = bt.run_day(day);
      std::printf("  %s market=%-4s news=%-5zu msgs=%-10zu signals=%-4zu alerts=%-3zu trades=%-3zu pnl=%9.2f  %.1fs %s\n",
                  r.day.c_str(), r.market.c_str(), r.news, r.market_messages, r.signals, r.alerts, r.trades_closed,
                  r.pnl, r.seconds, r.note.c_str());
      run.signals += r.signals;
      run.alerts += r.alerts;
      ++run.days;
    }
    const auto data = research::load_journal(o.out_dir);
    run.all = research::trade_stats(data.trades);
    if (!split_day.empty()) {
      run.in = stats_between(data, "", split_day);
      run.out = stats_between(data, split_day, "");
    }
    if (sweep_key.empty()) std::fputs(("\n" + research::render_report(data)).c_str(), stdout);
    runs.push_back(run);
  }

  if (!sweep_key.empty()) {
    std::printf("\n== Sweep %s ==\n", sweep_key.c_str());
    std::printf("%-28s %6s %6s %6s %6s %10s %6s %10s %9s %10s", "value", "days", "alerts", "trades", "win%",
                "expect$", "PF", "total$", "maxDD$", "ex-top3$");
    if (!split_day.empty()) std::printf(" | %8s %8s", "IS PF", "OOS PF");
    std::printf("\n");
    for (const auto& r : runs) {
      std::printf("%-28s %6zu %6zu %6zu %5.0f%% %10.2f %6.2f %10.2f %9.2f %10.2f", r.label.c_str(), r.days, r.alerts,
                  r.all.n, r.all.win_rate * 100, r.all.expectancy, r.all.profit_factor, r.all.total,
                  r.all.max_drawdown, r.all.total_ex_top3);
      if (!split_day.empty()) std::printf(" | %8.2f %8.2f", r.in.profit_factor, r.out.profit_factor);
      std::printf("\n");
    }
    std::printf("\nPick parameters on the in-sample period only; judge them on out-of-sample (--split).\n"
                "The best row of a sweep is optimistic by construction.\n");
  }
  Logger::instance().stop();
  return 0;
}
