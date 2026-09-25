#include "research/backtest.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>

#include "core/ascii.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "market/baseline.hpp"
#include "market/itch_book.hpp"
#include "market/itch_directory.hpp"
#include "market/sources.hpp"
#include "research/news_loader.hpp"
#include "sink/journal.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

namespace stok::research {

std::string itch_name_date(const std::string& ymd) {
  if (ymd.size() != 10) return {};
  return ymd.substr(5, 2) + ymd.substr(8, 2) + ymd.substr(0, 4);
}

namespace {

std::string first_existing(std::initializer_list<std::string> paths) {
  for (const auto& p : paths)
    if (fileutil::exists(p)) return p;
  return {};
}

// The simulation driver shared by the ITCH and tape paths.
class DayRun {
 public:
  DayRun(const BacktestOptions& o, Engine& engine, Journal& journal, SpscQueue<Signal>& alerts,
         SpscQueue<JournalRecord>& jq, SpscQueue<MarketEvent>& mq, std::vector<NewsEvent>& news, DayResult& res)
      : o_(o), engine_(engine), journal_(journal), alerts_(alerts), jq_(jq), mq_(mq), news_(news), res_(res) {}

  // Runs everything scheduled at or before simulated time t (epoch ns).
  void advance(uint64_t t) {
    if (t < next_due_) return;  // fast path: nothing due yet
    for (;;) {
      const uint64_t nn = ni_ < news_.size() ? news_[ni_].recv_ns : UINT64_MAX;
      if (!started_) {
        next_eval_ = std::min(nn, t);
        next_scan_ = next_eval_;
        started_ = true;
      }
      // An idle engine has nothing to evaluate: skip ahead instead of ticking.
      if (engine_.idle() && next_eval_ < std::min(nn, t)) {
        const uint64_t target = std::min(nn, t);
        const uint64_t steps = (target - next_eval_) / o_.eval_interval_ns;
        next_eval_ += steps * o_.eval_interval_ns;
      }
      uint64_t next = std::min(nn, next_eval_);
      if (o_.movers) next = std::min(next, next_scan_);
      if (next > t) {
        next_due_ = next;
        return;
      }
      if (next == nn) {
        engine_.on_news(news_[ni_], nn);
        ++ni_;
        ++res_.news;
      } else if (next == next_eval_) {
        engine_.evaluate(next_eval_);
        next_eval_ += o_.eval_interval_ns;
      } else {
        engine_.scan_movers(next_scan_);
        next_scan_ += 1'000'000'000ull;
      }
      drain();
    }
  }

  void market_events(uint64_t t) {
    while (MarketEvent* e = mq_.front()) {
      engine_.on_market_event(*e, t);
      mq_.pop();
    }
    drain();
  }

  void drain() {
    while (alerts_.front()) alerts_.pop();  // every signal is also journaled
    while (JournalRecord* r = jq_.front()) {
      if (r->type == JournalRecord::Type::Signal) {
        ++res_.signals;
        if (r->signal.tier == Tier::Alert || r->signal.tier == Tier::High) ++res_.alerts;
      }
      if (r->type == JournalRecord::Type::Trade && r->trade.closed) {
        ++res_.trades_closed;
        res_.pnl += r->trade.pnl_usd;
      }
      journal_.write(*r);
      jq_.pop();
    }
  }

  void finish(uint64_t t) {
    next_due_ = 0;
    advance(t);
    engine_.shutdown(t);
    drain();
  }

 private:
  const BacktestOptions& o_;
  Engine& engine_;
  Journal& journal_;
  SpscQueue<Signal>& alerts_;
  SpscQueue<JournalRecord>& jq_;
  SpscQueue<MarketEvent>& mq_;
  std::vector<NewsEvent>& news_;
  DayResult& res_;
  std::size_t ni_ = 0;
  bool started_ = false;
  uint64_t next_eval_ = 0, next_scan_ = 0, next_due_ = 0;
};

// Wraps the ITCH book so every message first advances the simulated clock.
struct TimedItch {
  ItchBook& book;
  DayRun& run;
  uint64_t midnight;
  uint64_t last = 0;
  STOK_ALWAYS_INLINE void tick(uint64_t ts) {
    last = midnight + ts;
    run.advance(last);
  }
  void on_system_event(uint64_t ts, char c) {
    tick(ts);
    book.on_system_event(ts, c);
    run.market_events(last);
  }
  void on_stock_directory(uint16_t l, uint64_t ts, uint64_t k, char c, char f, uint32_t r) {
    book.on_stock_directory(l, ts, k, c, f, r);
  }
  void on_trading_action(uint16_t l, uint64_t ts, uint64_t k, char s, const char* r) {
    tick(ts);
    book.on_trading_action(l, ts, k, s, r);
    run.market_events(last);
  }
  void on_reg_sho(uint16_t l, uint64_t ts, char a) { book.on_reg_sho(l, ts, a); }
  void on_add(uint16_t l, uint64_t ts, uint64_t ref, char s, uint32_t q, uint32_t p) {
    tick(ts);
    book.on_add(l, ts, ref, s, q, p);
  }
  void on_executed(uint16_t l, uint64_t ts, uint64_t ref, uint32_t q) {
    tick(ts);
    book.on_executed(l, ts, ref, q);
  }
  void on_executed_price(uint16_t l, uint64_t ts, uint64_t ref, uint32_t q, bool pr, uint32_t p) {
    tick(ts);
    book.on_executed_price(l, ts, ref, q, pr, p);
  }
  void on_cancel(uint16_t l, uint64_t ts, uint64_t ref, uint32_t q) {
    tick(ts);
    book.on_cancel(l, ts, ref, q);
  }
  void on_delete(uint16_t l, uint64_t ts, uint64_t ref) {
    tick(ts);
    book.on_delete(l, ts, ref);
  }
  void on_replace(uint16_t l, uint64_t ts, uint64_t o, uint64_t n, uint32_t q, uint32_t p) {
    tick(ts);
    book.on_replace(l, ts, o, n, q, p);
  }
  void on_trade(uint16_t l, uint64_t ts, char s, uint32_t q, uint64_t k, uint32_t p) {
    tick(ts);
    book.on_trade(l, ts, s, q, k, p);
  }
  void on_cross(uint16_t l, uint64_t ts, uint64_t q, uint64_t k, uint32_t p, char c) {
    tick(ts);
    book.on_cross(l, ts, q, k, p, c);
  }
  void on_broken(uint16_t l, uint64_t ts, uint64_t m) { book.on_broken(l, ts, m); }
};

struct TapeState {
  SpscQueue<MarketEvent>* mq;
};

void tape_state(void* ctx, uint32_t sym, char state, const char* reason, int64_t ts) {
  auto* st = static_cast<TapeState*>(ctx);
  if (MarketEvent* e = st->mq->try_claim()) {
    *e = MarketEvent{};
    e->type = MarketEvent::Type::TradingAction;
    e->state = state;
    int n = 0;
    for (; n < 4 && reason[n] && reason[n] != ' '; ++n) e->reason[n] = reason[n];
    e->reason[n] = '\0';
    e->sym = sym;
    e->ts_ns = ts;
    st->mq->publish();
  }
}

}  // namespace

Backtester::Backtester(BacktestOptions opts) : opts_(std::move(opts)) {}

bool Backtester::init(std::string* err) {
  std::string rep;
  if (!opts_.ref_dir.empty()) base_.load_dir(opts_.ref_dir, &rep);
  // No live baseline: prev close / ADV must come from days before each replayed day.
  if (!scorer_.load_rules_file(opts_.rules_file, err)) return false;
  if (!opts_.filings_path.empty()) {
    if (auto t = fileutil::read_file(opts_.filings_path)) filings_.load_tsv(*t);
  }
  if (opts_.days.empty()) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(opts_.news_dir, ec)) {
      const std::string d = e.path().filename().string();
      int y;
      unsigned m, dd;
      if (!e.is_directory() || !timeutil::parse_ymd(d, y, m, dd)) continue;
      if (!opts_.from.empty() && d < opts_.from) continue;
      if (!opts_.to.empty() && d > opts_.to) continue;
      if (fileutil::exists(fileutil::join(e.path().string(), "news.jsonl"))) days_.push_back(d);
    }
    std::sort(days_.begin(), days_.end());
  } else {
    days_ = opts_.days;
  }
  if (days_.empty()) {
    if (err) *err = "no days with news.jsonl in " + opts_.news_dir;
    return false;
  }
  fileutil::make_dirs(opts_.out_dir);
  return true;
}

std::vector<DayResult> Backtester::run() {
  std::vector<DayResult> out;
  for (const auto& d : days_) out.push_back(run_day(d));
  return out;
}

DayResult Backtester::run_day(const std::string& day) {
  DayResult res;
  res.day = day;
  const uint64_t t0 = mono_ns();
  int y;
  unsigned m, d;
  if (!timeutil::parse_ymd(day, y, m, d)) {
    res.note = "bad date";
    return res;
  }
  // ---- market input ----
  const std::string nd = itch_name_date(day);
  const std::string itch = opts_.itch_dir.empty() ? std::string() : first_existing({
      fileutil::join(opts_.itch_dir, nd + ".NASDAQ_ITCH50.gz"), fileutil::join(opts_.itch_dir, nd + ".NASDAQ_ITCH50"),
      fileutil::join(opts_.itch_dir, nd + ".NASDAQ_ITCH50.bin")});
  const std::string tape = opts_.tape_dir.empty() ? std::string() : first_existing({
      fileutil::join(opts_.tape_dir, day + ".tape.gz"), fileutil::join(opts_.tape_dir, day + ".tape")});
  res.market = !itch.empty() ? "itch" : !tape.empty() ? "tape" : "none";

  // ---- symbols as of this day ----
  SymbolTable syms = base_;
  if (!itch.empty()) add_itch_directory(itch, syms);
  for (uint32_t i = 0; i < syms.size(); ++i) {
    syms.mut(i).prev_close = 0;
    syms.mut(i).adv20 = 0;
    syms.mut(i).adv_days = 0;
  }
  const std::string baseline_dir = fileutil::join(opts_.out_dir, "baseline");
  if (opts_.rolling_baseline) {
    if (auto b = fileutil::read_file(fileutil::join(baseline_dir, "baseline.tsv"))) syms.load_baseline(*b);
  }
  syms.build_name_index();

  // ---- news ----
  auto loaded = load_news_file(fileutil::join(fileutil::join(opts_.news_dir, day), "news.jsonl"), syms, sources_);
  std::vector<NewsEvent>& news = loaded.events;
  if (opts_.news_delay_ms != 0) {
    for (auto& e : news) {
      const int64_t shifted = static_cast<int64_t>(e.recv_ns) + opts_.news_delay_ms * 1'000'000;
      e.recv_ns = static_cast<uint64_t>(std::max<int64_t>(0, shifted));
      e.parsed_ns = e.recv_ns;
    }
  }
  if (loaded.dropped_tickers) res.note += std::to_string(loaded.dropped_tickers) + " unknown tickers; ";

  // ---- engine ----
  MarketBoard board(syms.size());
  board.set_session_date(y, m, d);
  const uint64_t midnight = static_cast<uint64_t>(board.midnight_ns());
  EngineConfig ec = opts_.engine;
  ec.sim_time = true;
  ec.movers_enabled = opts_.movers;
  SpscQueue<Signal> alerts(1 << 12);
  SpscQueue<JournalRecord> jq(1 << 12);
  SpscQueue<MarketEvent> mq(1 << 16);
  // Filings that arrive during the replay are learned by the engine; later
  // days see them too, exactly like a live daemon running across days.
  // Only filings from earlier days are known at the open; same-day filings
  // arrive as timed events (EDGAR feed / history) like they do live.
  FilingsHistory day_filings = filings_.before(static_cast<int32_t>(timeutil::days_from_civil(y, m, d)));
  Engine engine(ec, syms, res.market == "none" ? nullptr : &board, scorer_, day_filings, &alerts, nullptr, &jq, nullptr,
                sources_);
  Journal journal(opts_.out_dir, syms, &scorer_, sources_);
  DayRun run(opts_, engine, journal, alerts, jq, mq, news, res);

  if (!itch.empty()) {
    ItchBook book(syms, board, &mq, nullptr, opts_.order_capacity, true);
    if (!opts_.movers) {
      // Quotes only for tickers with news today (volume is still tracked for
      // every symbol, so the rolling baseline stays exact).
      std::vector<uint8_t> mask(syms.size(), 0);
      for (const auto& e : news)
        for (int k = 0; k < e.n_tickers; ++k) mask[e.tickers[k]] = 1;
      book.set_quote_filter(std::move(mask));
    }
    ItchFileReader reader;
    std::string err;
    if (!reader.open(itch, &err)) {
      res.note += err;
      return res;
    }
    TimedItch timed{book, run, midnight};
    std::atomic<bool> stop{false};
    while (reader.run(timed, stop, 0.0, 1 << 20)) {
    }
    res.market_messages = reader.stats().messages;
    run.finish(std::max<uint64_t>(timed.last, midnight + 20ull * 3600 * kNsPerSec));
  } else if (!tape.empty()) {
    // Tape lines: "<epoch_ns> <bridge line>" (see MarketRecorder).
    gzFile gz = gzopen(tape.c_str(), "rb");
    BridgeReceiver bridge;
    TapeState st{&mq};
    char buf[1024];
    uint64_t last = midnight;
    while (gz && gzgets(gz, buf, sizeof(buf))) {
      std::string_view line = trim(std::string_view(buf));
      const std::size_t sp = line.find(' ');
      if (sp == std::string_view::npos) continue;
      const auto ts = parse_int<int64_t>(line.substr(0, sp));
      if (!ts) continue;
      last = static_cast<uint64_t>(*ts);
      run.advance(last);
      bridge.handle(line.substr(sp + 1), syms, board, &tape_state, &st, *ts);
      ++res.market_messages;
      if (mq.front()) run.market_events(last);
    }
    if (gz) gzclose(gz);
    run.finish(std::max<uint64_t>(last, midnight + 20ull * 3600 * kNsPerSec));
  } else {
    // News-only replay: scoring and WATCH signals, no prices or trades.
    run.finish(midnight + 24ull * 3600 * kNsPerSec - 1);
    res.note += "no market data for this day (news-only); ";
  }
  journal.flush();
  filings_.merge(day_filings);  // keep what the engine learned for later days
  if (opts_.rolling_baseline && res.market != "none") {
    BaselineStore store(baseline_dir);
    if (store.append_session(day, board, syms)) store.rebuild(20);
  }
  res.seconds = static_cast<double>(mono_ns() - t0) / 1e9;
  return res;
}

}  // namespace stok::research
