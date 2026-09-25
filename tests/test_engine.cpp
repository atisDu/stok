#include <vector>

#include "check.hpp"
#include "core/clock.hpp"
#include "engine/engine.hpp"
#include "feeds/parsers.hpp"
#include "fixture_symbols.hpp"
#include "sink/alerts.hpp"
#include "sink/journal.hpp"
#include "util/json.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

struct Rig {
  SymbolTable syms = test::fixture_symbols();
  MarketBoard board{syms.size()};
  Scorer scorer;
  FilingsHistory filings;
  SpscQueue<Signal> alerts{256};
  SpscQueue<JournalRecord> journal{512};
  std::vector<std::string> sources{"globenewswire", "prnewswire", "edgar_8k", "halts"};
  std::unique_ptr<Engine> engine;
  int64_t midnight = 0;
  uint64_t now = 0;

  explicit Rig(EngineConfig cfg = {}) {
    std::string err;
    if (!scorer.load_rules_file(test::source_path("config/rules.tsv"), &err)) test::fail(__FILE__, __LINE__, err);
    now = wall_ns();
    int y;
    unsigned m, d;
    timeutil::eastern_date(static_cast<int64_t>(now), y, m, d);
    board.set_session_date(y, m, d);
    midnight = board.midnight_ns();
    // RVOL depends on the wall-clock time of day; disable it here (unknown
    // baseline volume never blocks an alert) so the test is time-independent.
    for (uint32_t i = 0; i < syms.size(); ++i) syms.mut(i).adv20 = 0;
    engine = std::make_unique<Engine>(cfg, syms, &board, scorer, filings, &alerts, nullptr, &journal, nullptr, sources);
  }
  uint64_t since_midnight() const { return now - static_cast<uint64_t>(midnight); }
  void trade(const char* t, double px, uint64_t sh) {
    board.on_trade(syms.find(t), static_cast<int32_t>(px * 1e4 + 0.5), sh, since_midnight());
  }
  std::vector<Signal> drain_alerts() {
    std::vector<Signal> out;
    while (Signal* s = alerts.front()) {
      out.push_back(*s);
      alerts.pop();
    }
    return out;
  }
  std::vector<JournalRecord::Type> drain_journal(std::vector<Signal>* signals = nullptr) {
    std::vector<JournalRecord::Type> out;
    while (JournalRecord* r = journal.front()) {
      out.push_back(r->type);
      if (signals && r->type == JournalRecord::Type::Signal) signals->push_back(r->signal);
      journal.pop();
    }
    return out;
  }
};

std::vector<NewsEvent> events(FeedKind kind, const std::string& fixture, const SymbolTable& syms, uint16_t source,
                              uint64_t recv) {
  std::vector<NewsEvent> out;
  ParseScratch s;
  scan_feed(kind, test::fixture(fixture), [&](std::string_view item, uint64_t id) {
    NewsEvent ev;
    ev.reset();
    if (!fill_event(kind, item, syms, ev, s)) return;
    ev.id_hash = id;
    ev.source = source;
    ev.recv_ns = recv;
    ev.parsed_ns = recv;
    out.push_back(ev);
  });
  return out;
}

}  // namespace

TEST(engine_news_to_watch_to_alert) {
  Rig r;
  r.trade("ACMR", 2.00, 1000);  // last print before the news
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  CHECK_EQ(gnw.size(), 3u);
  if (gnw.size() < 3) return;

  r.engine->on_news(gnw[0], r.now);  // ACMR: $20M Walmart supply agreement
  CHECK_EQ(r.engine->watch_count(), 1u);
  CHECK(r.drain_alerts().empty());     // WATCH is journal-only by default
  std::vector<Signal> js;
  auto types = r.drain_journal(&js);
  CHECK_EQ(js.size(), 1u);
  if (!js.empty()) {
    CHECK(js[0].tier == Tier::Watch);
    CHECK_EQ(std::string(js[0].ticker), std::string("ACMR"));
    CHECK(js[0].score >= 55);
    CHECK(js[0].why.view().find("contract") == 0);
  }

  // The stock runs +20% on $480k volume -> price confirmation.
  r.trade("ACMR", 2.40, 200000);
  r.engine->evaluate(r.now + 1'000'000);
  auto al = r.drain_alerts();
  CHECK_EQ(al.size(), 1u);
  if (!al.empty()) {
    CHECK(al[0].tier == Tier::Alert || al[0].tier == Tier::High);
    CHECK_NEAR(al[0].move_pct, 20.0, 1e-6);
    CHECK(al[0].dollar_volume >= 480000.0);
    CHECK(al[0].market_cap > 0);
    const std::string text = AlertSink::format_text(al[0], r.sources);
    CHECK(text.find("ACMR") != std::string::npos);
    CHECK(text.find("since news") != std::string::npos);
  }
  // No duplicate alert on the next evaluation.
  r.engine->evaluate(r.now + 2'000'000);
  CHECK(r.drain_alerts().empty());
  r.drain_journal();
}

TEST(engine_blocks_offering_and_spam) {
  Rig r;
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.size() < 3) return;
  r.engine->on_news(gnw[1], r.now);  // BRVO registered direct offering
  r.engine->on_news(gnw[2], r.now);  // law-firm alert about CHLY
  CHECK_EQ(r.engine->watch_count(), 0u);
  auto types = r.drain_journal();
  CHECK_EQ(types.size(), 2u);  // both journaled as news, no signals
}

TEST(engine_recent_424b_blocks_even_good_news) {
  Rig r;
  int y;
  unsigned m, d;
  timeutil::eastern_date(static_cast<int64_t>(r.now), y, m, d);
  const int32_t today = static_cast<int32_t>(timeutil::days_from_civil(y, m, d));
  r.filings.add(1234567, today - 2, FormClass::Prospectus);  // ACMR sold stock 2 days ago
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  CHECK_EQ(r.engine->watch_count(), 0u);
}

TEST(engine_halts_movers_outcomes_and_first_seen) {
  EngineConfig cfg;
  cfg.push_watch = true;
  Rig r(cfg);
  r.trade("ACMR", 2.00, 1000);
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  auto first = r.drain_alerts();
  CHECK_EQ(first.size(), 1u);  // WATCH pushed because push_watch = true

  // Halt on the watched ticker (exchange feed) -> HALT signal; the same halt
  // from the RSS feed moments later is deduplicated.
  MarketEvent me{};
  me.type = MarketEvent::Type::TradingAction;
  me.state = 'H';
  std::snprintf(me.reason, sizeof(me.reason), "LUDP");
  me.sym = r.syms.find("ACMR");
  me.ts_ns = static_cast<int64_t>(r.now);
  r.engine->on_market_event(me, r.now);
  const auto halts = events(FeedKind::NasdaqHalts, "halts.xml", r.syms, 3, r.now + 1000);
  if (!halts.empty()) r.engine->on_news(halts[0], r.now + 1000);
  auto hs = r.drain_alerts();
  CHECK_EQ(hs.size(), 1u);
  if (!hs.empty()) {
    CHECK(hs[0].tier == Tier::Halt);
    CHECK_EQ(std::string(hs[0].halt_reason), std::string("LUDP"));
  }

  // Same headline arriving later from EDGAR (exhibit) counts as "behind".
  NewsEvent dup = gnw[0];
  dup.source = 2;
  dup.recv_ns = r.now + 250'000'000;
  r.engine->on_news(dup, r.now + 250'000'000);
  CHECK_EQ(r.engine->source_stats(2).dup, 1u);
  CHECK_EQ(r.engine->source_stats(0).first, 1u);

  // DLTX +40% on $2.2M with no news -> MOVER.
  r.trade("DLTX", 5.60, 400000);
  r.engine->scan_movers(r.now);
  r.engine->scan_movers(r.now + 1);  // once per day
  auto mv = r.drain_alerts();
  CHECK_EQ(mv.size(), 1u);
  if (!mv.empty()) {
    CHECK(mv[0].tier == Tier::Mover);
    CHECK_EQ(std::string(mv[0].ticker), std::string("DLTX"));
  }

  // Outcome records at the +60s horizon.
  r.drain_journal();
  r.engine->evaluate(r.now + 61ull * 1'000'000'000ull);
  int outcomes = 0;
  for (auto t : r.drain_journal())
    if (t == JournalRecord::Type::Outcome) ++outcomes;
  CHECK_EQ(outcomes, 1);
}

TEST(engine_wide_spread_blocks_alert) {
  Rig r;
  r.trade("ACMR", 2.00, 1000);
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  const uint32_t acmr = r.syms.find("ACMR");
  r.board.on_quote(acmr, 22000, 1000, 26000, 1000, r.since_midnight());  // 16.7% spread
  r.trade("ACMR", 2.40, 200000);
  r.engine->evaluate(r.now + 1);
  CHECK(r.drain_alerts().empty());
  r.board.on_quote(acmr, 23900, 5000, 24100, 3000, r.since_midnight());  // 0.8% spread
  r.engine->evaluate(r.now + 2);
  auto al = r.drain_alerts();
  CHECK_EQ(al.size(), 1u);
  if (!al.empty()) {
    CHECK_NEAR(al[0].spread_pct, 200.0 * 200 / 48000.0, 1e-9);
    CHECK_NEAR(al[0].ask, 2.41, 1e-9);
  }
}

TEST(engine_cooldown_suppresses_repeat_signals) {
  EngineConfig cfg;
  cfg.watch_window_s = 1;
  cfg.cooldown_s = 1800;
  Rig r(cfg);
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  r.engine->evaluate(r.now + 2'000'000'000ull);  // watch window over: entry expires
  CHECK_EQ(r.engine->watch_count(), 0u);
  NewsEvent again = gnw[0];
  again.id_hash ^= 1;
  again.title_key ^= 1;
  again.recv_ns = r.now + 3'000'000'000ull;
  r.engine->on_news(again, again.recv_ns);  // same ticker 3 s later, inside cooldown
  CHECK_EQ(r.engine->watch_count(), 1u);    // tracked again...
  CHECK_EQ(r.engine->signals_emitted(Tier::Watch), 1u);  // ...but not re-signaled
}

TEST(engine_paper_trade_from_signal) {
  EngineConfig cfg;
  cfg.paper.enabled = true;
  cfg.paper.latency_ms = 0;
  cfg.paper.slippage_bps = 0;
  cfg.paper.flat_by_minute = 24 * 60;
  Rig r(cfg);
  r.trade("ACMR", 2.00, 1000);
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  const uint32_t acmr = r.syms.find("ACMR");
  r.board.on_quote(acmr, 23900, 5000, 24100, 3000, r.since_midnight());
  r.trade("ACMR", 2.40, 200000);
  r.engine->evaluate(r.now + 1);   // ALERT/HIGH -> paper order
  r.engine->evaluate(r.now + 2);   // fill
  r.board.on_quote(acmr, 29500, 5000, 29700, 3000, r.since_midnight());
  r.engine->evaluate(r.now + 3);   // +22% at the bid -> target
  r.engine->shutdown(r.now + 4);
  int opens = 0, closes = 0;
  double pnl = 0;
  while (JournalRecord* rec = r.journal.front()) {
    if (rec->type == JournalRecord::Type::Trade) {
      if (rec->trade.closed) {
        ++closes;
        pnl = rec->trade.pnl_usd;
        CHECK_EQ(std::string(rec->trade.exit_reason), std::string("target"));
      } else {
        ++opens;
        CHECK_NEAR(rec->trade.entry_px, 2.41, 1e-9);
      }
    }
    r.journal.pop();
  }
  CHECK_EQ(opens, 1);
  CHECK_EQ(closes, 1);
  CHECK(pnl > 200);
  CHECK(r.engine->paper() != nullptr && r.engine->paper()->stats().wins == 1);
}

TEST(journal_lines_are_valid_json) {
  Rig r;
  r.trade("ACMR", 2.00, 1000);
  const auto gnw = events(FeedKind::Rss, "globenewswire.xml", r.syms, 0, r.now);
  if (gnw.empty()) return;
  r.engine->on_news(gnw[0], r.now);
  r.trade("ACMR", 2.40, 200000);
  r.engine->evaluate(r.now + 1);
  r.engine->evaluate(r.now + 3601ull * 1'000'000'000ull);
  Journal j("build/test_tmp/journal", r.syms, &r.scorer, r.sources);
  int n = 0;
  while (JournalRecord* rec = r.journal.front()) {
    std::string line;
    if (rec->type == JournalRecord::Type::News) line = j.news_json(*rec);
    else if (rec->type == JournalRecord::Type::Signal) line = j.signal_json(rec->signal);
    else if (rec->type == JournalRecord::Type::Outcome) line = j.outcome_json(rec->outcome);
    else if (rec->type == JournalRecord::Type::Trade) line = j.trade_json(rec->trade);
    JsonDoc doc;
    CHECK(doc.parse(line));
    if (rec->type == JournalRecord::Type::News) {
      CHECK_EQ(JsonDoc::str(doc.get(doc.root(), "sym")), std::string("ACMR"));
      CHECK_EQ(JsonDoc::str(doc.get(doc.root(), "catalyst")), std::string("contract"));
    }
    j.write(*rec);
    r.journal.pop();
    ++n;
  }
  j.flush();
  CHECK(n >= 3);
}
