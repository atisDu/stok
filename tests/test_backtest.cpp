#include <filesystem>
#include <map>
#include <vector>

#include "check.hpp"
#include "engine/engine.hpp"
#include "feeds/parsers.hpp"
#include "fixture_symbols.hpp"
#include "research/edgar_history.hpp"
#include "research/news_loader.hpp"
#include "sink/journal.hpp"

using namespace stok;

namespace {

std::vector<NewsEvent> parse_fixture(FeedKind kind, const char* name, const SymbolTable& syms, uint16_t src,
                                     uint64_t recv) {
  std::vector<NewsEvent> out;
  ParseScratch s;
  scan_feed(kind, test::fixture(name), [&](std::string_view item, uint64_t id) {
    NewsEvent ev;
    ev.reset();
    if (!fill_event(kind, item, syms, ev, s)) return;
    ev.id_hash = id;
    ev.source = src;
    ev.recv_ns = recv + out.size() * 1000 + 123;  // odd ns: checks exact 64-bit round trip
    ev.sent_ns = ev.recv_ns - 5'000'000;
    out.push_back(ev);
  });
  return out;
}

}  // namespace

TEST(news_journal_roundtrip_for_replay) {
  const SymbolTable syms = test::fixture_symbols();
  const std::vector<std::string> names = {"globenewswire", "edgar_8k", "halts"};
  const uint64_t t = 1790337600123456789ull;
  std::vector<NewsEvent> orig;
  for (auto& e : parse_fixture(FeedKind::Rss, "globenewswire.xml", syms, 0, t)) orig.push_back(e);
  for (auto& e : parse_fixture(FeedKind::EdgarAtom, "edgar_current.xml", syms, 1, t + 50'000)) orig.push_back(e);
  for (auto& e : parse_fixture(FeedKind::NasdaqHalts, "halts.xml", syms, 2, t + 90'000)) orig.push_back(e);
  Journal j("build/test_tmp/unused", syms, nullptr, names);
  std::string jsonl;
  for (const auto& e : orig) {
    JournalRecord r{};
    r.type = JournalRecord::Type::News;
    r.news = e;
    r.news_sym = e.n_tickers ? e.tickers[0] : SymbolTable::kInvalid;
    jsonl += j.news_json(r) + "\n";
  }
  std::vector<std::string> sources;
  const auto loaded = research::load_news_events(jsonl, syms, sources);
  CHECK_EQ(loaded.bad_lines, 0u);
  CHECK_EQ(loaded.events.size(), orig.size());
  CHECK_EQ(sources.size(), 3u);
  for (std::size_t i = 0; i < orig.size() && i < loaded.events.size(); ++i) {
    const NewsEvent& a = orig[i];
    const NewsEvent& b = loaded.events[i];
    CHECK(a.kind == b.kind);
    CHECK_EQ(a.recv_ns, b.recv_ns);
    CHECK_EQ(a.id_hash, b.id_hash);
    CHECK_EQ(a.title_key, b.title_key);
    CHECK_EQ(std::string(a.title.view()), std::string(b.title.view()));
    CHECK_EQ(std::string(a.body.view()), std::string(b.body.view()));  // full body, not truncated
    CHECK_EQ(a.n_tickers, b.n_tickers);
    for (int k = 0; k < a.n_tickers && k < b.n_tickers; ++k) CHECK_EQ(a.tickers[k], b.tickers[k]);
    CHECK_EQ(a.items_mask, b.items_mask);
    CHECK_EQ(a.cik, b.cik);
    CHECK_EQ(std::string(a.form), std::string(b.form));
    CHECK_EQ(a.n_unresolved, b.n_unresolved);
    CHECK_EQ(a.halt.halt_ns, b.halt.halt_ns);
    CHECK_EQ(a.halt.resume_trade_ns, b.halt.resume_trade_ns);
    CHECK_EQ(std::string(a.halt.reason), std::string(b.halt.reason));
    CHECK_EQ(names[a.source], sources[b.source]);
  }
}

// ---------------------------------------------------------------------------
// End-to-end backtest on a synthetic day.

#include <zlib.h>

#include "market/itch.hpp"
#include "research/backtest.hpp"
#include "research/report.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

namespace {

namespace enc = stok::itch::enc;
constexpr uint64_t kMinNs = 60'000'000'000ull;
uint64_t et(int h, int m, int s = 0, int ms = 0) {
  return (static_cast<uint64_t>(h) * 60 + m) * kMinNs + static_cast<uint64_t>(s) * 1'000'000'000ull +
         static_cast<uint64_t>(ms) * 1'000'000ull;
}

struct Scenario {
  std::string root = "build/test_tmp/backtest";
  std::string ref, news, itch, tape;
  int64_t midnight = timeutil::eastern_midnight_ns(2026, 9, 25);

  Scenario() {
    std::filesystem::remove_all(root);
    ref = root + "/ref";
    news = root + "/news";
    itch = root + "/itch";
    tape = root + "/tape";
    for (const auto& d : {ref, news + "/2026-09-25", itch, tape}) fileutil::make_dirs(d);
    for (const char* f : {"nasdaqlisted.txt", "otherlisted.txt", "company_tickers_exchange.json"})
      fileutil::write_file_atomic(ref + "/" + f, test::fixture(f));
    fileutil::write_file_atomic(ref + "/fundamentals.tsv",
                                "cik\tshares_outstanding\tshares_asof\tpublic_float_usd\tfloat_asof\n"
                                "1234567\t12000000\t2026-08-01\t0\t\n");
    write_news();
    write_itch();
    write_tape();
  }

  void write_news() {
    SymbolTable syms = test::fixture_symbols();
    ParseScratch s;
    std::string jsonl;
    Journal j(root + "/unused", syms, nullptr, {"globenewswire"});
    scan_feed(FeedKind::Rss, test::fixture("globenewswire.xml"), [&](std::string_view item, uint64_t id) {
      NewsEvent ev;
      ev.reset();
      if (!fill_event(FeedKind::Rss, item, syms, ev, s)) return;
      ev.id_hash = id;
      ev.source = 0;
      ev.recv_ns = static_cast<uint64_t>(midnight) + et(10, 0, 0, 500);  // 10:00:00.500 ET
      JournalRecord r{};
      r.type = JournalRecord::Type::News;
      r.news = ev;
      r.news_sym = ev.n_tickers ? ev.tickers[0] : SymbolTable::kInvalid;
      jsonl += j.news_json(r) + "\n";
    });
    fileutil::write_file_atomic(news + "/2026-09-25/news.jsonl", jsonl);
  }

  void write_itch() {
    gzFile gz = gzopen((itch + "/09252026.NASDAQ_ITCH50.gz").c_str(), "wb");
    uint8_t m[64];
    auto put = [&](std::size_t n) {
      uint8_t len[2];
      enc::put16(len, static_cast<uint16_t>(n));
      gzwrite(gz, len, 2);
      gzwrite(gz, m, static_cast<unsigned>(n));
    };
    put(enc::system_event(m, et(3, 0), 'O'));
    put(enc::stock_directory(m, 1, et(3, 0), "ACMR"));
    put(enc::stock_directory(m, 2, et(3, 0), "BRVO"));
    put(enc::add(m, 1, et(9, 30), 1, 'B', 3000, "ACMR", 19900));
    put(enc::add(m, 1, et(9, 30), 2, 'S', 2500, "ACMR", 20100));
    put(enc::trade(m, 1, et(9, 45), 'B', 1000, "ACMR", 20000));           // last print before the news
    put(enc::add(m, 2, et(9, 50), 9, 'B', 100, "BRVO", 25000));
    // After the 10:00:00.5 story: the book moves up and it trades $480K.
    put(enc::del(m, 1, et(10, 0, 1), 1));
    put(enc::del(m, 1, et(10, 0, 1), 2));
    put(enc::add(m, 1, et(10, 0, 1), 3, 'B', 5000, "ACMR", 23900));
    put(enc::add(m, 1, et(10, 0, 1), 4, 'S', 3000, "ACMR", 24100));
    put(enc::trade(m, 1, et(10, 0, 2), 'B', 200000, "ACMR", 24000));
    // 10:05: runs to 2.96 -> +22% over a 2.41 entry.
    put(enc::del(m, 1, et(10, 5), 3));
    put(enc::del(m, 1, et(10, 5), 4));
    put(enc::add(m, 1, et(10, 5), 5, 'B', 5000, "ACMR", 29500));
    put(enc::add(m, 1, et(10, 5), 6, 'S', 2000, "ACMR", 29700));
    put(enc::trade(m, 1, et(10, 5, 1), 'B', 20000, "ACMR", 29600));
    put(enc::trade(m, 1, et(11, 30), 'B', 100, "ACMR", 29600));  // keeps outcomes flowing past +60m
    put(enc::system_event(m, et(20, 0), 'C'));
    gzclose(gz);
  }

  void write_tape() {
    std::string t;
    auto line = [&](uint64_t ns_since_midnight, const std::string& l) {
      t += std::to_string(static_cast<uint64_t>(midnight) + ns_since_midnight) + " " + l + "\n";
    };
    line(et(9, 30), "Q ACMR 1.99 3000 2.01 2500");
    line(et(9, 45), "T ACMR 2.00 1000");
    line(et(10, 0, 1), "Q ACMR 2.39 5000 2.41 3000");
    line(et(10, 0, 2), "T ACMR 2.40 200000");
    line(et(10, 5), "Q ACMR 2.95 5000 2.97 2000");
    line(et(10, 5, 1), "T ACMR 2.96 20000");
    fileutil::write_file_atomic(tape + "/2026-09-25.tape", t);
  }

  research::BacktestOptions opts(const std::string& out) const {
    research::BacktestOptions o;
    o.news_dir = news;
    o.itch_dir = itch;
    o.out_dir = root + "/" + out;
    o.ref_dir = ref;
    o.rules_file = test::source_path("config/rules.tsv");
    o.engine.paper.enabled = true;
    o.engine.paper.latency_ms = 250;
    o.engine.paper.slippage_bps = 30;
    o.engine.paper.flat_by_minute = 24 * 60;
    return o;
  }
};

std::vector<std::string> jsonl_lines(const std::string& path) {
  std::vector<std::string> out;
  if (auto t = fileutil::read_file(path)) for_each_line(*t, [&](std::string_view l) {
      if (!l.empty()) out.emplace_back(l);
    });
  return out;
}

research::DayResult run_one(research::BacktestOptions o) {
  research::Backtester bt(std::move(o));
  std::string err;
  if (!bt.init(&err)) {
    test::fail(__FILE__, __LINE__, "init: " + err);
    return {};
  }
  const auto r = bt.run();
  return r.empty() ? research::DayResult{} : r[0];
}

}  // namespace

TEST(backtest_itch_day_end_to_end) {
  Scenario sc;
  const auto r = run_one(sc.opts("out_a"));
  CHECK_EQ(r.market, std::string("itch"));
  CHECK_EQ(r.news, 3u);
  CHECK(r.alerts >= 1);
  CHECK_EQ(r.trades_closed, 1u);
  CHECK(r.pnl > 100);
  const auto data = research::load_journal(sc.root + "/out_a");
  CHECK_EQ(data.trades.size(), 1u);
  if (!data.trades.empty()) {
    CHECK_EQ(data.trades[0].exit_reason, std::string("target"));
    CHECK_EQ(data.trades[0].ticker, std::string("ACMR"));
  }
  // Entry: the ask (2.41) + 30 bps, 250 ms after the 10:00:02 confirmation.
  const auto trades = jsonl_lines(sc.root + "/out_a/2026-09-25/trades.jsonl");
  CHECK(!trades.empty() && trades[0].find("\"entry_px\":2.4172") != std::string::npos);
  // Outcomes were recorded at the configured horizons.
  bool h60 = false, h3600 = false;
  for (const auto& o : data.outcomes) {
    h60 |= o.horizon_s == 60;
    h3600 |= o.horizon_s == 3600;
  }
  CHECK(h60 && h3600);
  // Offering story (BRVO) and law-firm spam (CHLY) never became signals.
  for (const auto& s : jsonl_lines(sc.root + "/out_a/2026-09-25/signals.jsonl"))
    CHECK(s.find("\"ticker\":\"ACMR\"") != std::string::npos);
  // Rolling baseline written for the next day.
  CHECK(fileutil::exists(sc.root + "/out_a/baseline/baseline.tsv"));
}

TEST(backtest_is_deterministic) {
  Scenario sc;
  run_one(sc.opts("out_1"));
  run_one(sc.opts("out_2"));
  for (const char* f : {"signals.jsonl", "trades.jsonl", "outcomes.jsonl", "news.jsonl"}) {
    const auto a = fileutil::read_file(sc.root + "/out_1/2026-09-25/" + f);
    const auto b = fileutil::read_file(sc.root + "/out_2/2026-09-25/" + f);
    CHECK(a.has_value() && b.has_value());
    if (a && b) CHECK(*a == *b);
  }
}

TEST(backtest_news_latency_sensitivity) {
  Scenario sc;
  auto o = sc.opts("out_late");
  o.news_delay_ms = 5 * 60 * 1000;  // the same story, received 5 minutes later
  const auto r = run_one(o);
  // By 10:05:00.5 the move already happened: reference price is 2.96, no confirmation, no trade.
  CHECK_EQ(r.trades_closed, 0u);
  CHECK_EQ(r.alerts, 0u);
}

TEST(backtest_from_bridge_tape) {
  Scenario sc;
  auto o = sc.opts("out_tape");
  o.itch_dir.clear();
  o.tape_dir = sc.tape;
  const auto r = run_one(o);
  CHECK_EQ(r.market, std::string("tape"));
  CHECK_EQ(r.trades_closed, 1u);
  CHECK(r.pnl > 100);
}

TEST(backtest_news_only_day) {
  Scenario sc;
  auto o = sc.opts("out_news_only");
  o.itch_dir.clear();
  const auto r = run_one(o);
  CHECK_EQ(r.market, std::string("none"));
  CHECK_EQ(r.news, 3u);
  CHECK_EQ(r.trades_closed, 0u);
  CHECK(r.signals >= 1);  // WATCH still recorded
}

// ---- EDGAR history (past days rebuilt from the SEC archives) ----

namespace {

constexpr const char* kAcmrIndex =
    "https://www.sec.gov/Archives/edgar/data/1234567/000123456726000012/0001234567-26-000012-index.htm";
constexpr const char* kAcmrEx99 = "https://www.sec.gov/Archives/edgar/data/1234567/000123456726000012/ex99-1.htm";

// A tiny offline "sec.gov": the day's master index, filing index pages and the exhibit.
struct FakeSec {
  std::map<std::string, std::string> pages;
  std::vector<std::string> fetched;

  explicit FakeSec(const std::string& acmr_accepted = "2026-09-25 09:59:00") {
    pages["https://www.sec.gov/Archives/edgar/daily-index/2026/QTR3/master.20260925.idx"] =
        "Description:           Daily Index of EDGAR Dissemination Feed by Company Name\n\n"
        "CIK|Company Name|Form Type|Date Filed|File Name\n"
        "--------------------------------------------------------------------------------\n"
        "1234567|ACME ROBOTICS, INC.|8-K|20260925|edgar/data/1234567/0001234567-26-000012.txt\n"
        "7654321|BRAVO BIO, INC.|424B5|20260925|edgar/data/7654321/0001104659-26-099999.txt\n"
        "2222222|DELTA THERAPEUTICS, INC.|4|20260925|edgar/data/2222222/0000000000-26-000001.txt\n"
        "4444444|ECHO MINING CORP|8-K|20260925|edgar/data/4444444/0004444444-26-000001.txt\n"
        "5555555|SOME CO|10-Q|20260925|edgar/data/5555555/0000000000-26-000003.txt\n";
    std::string idx = test::fixture("edgar_index.htm");
    const auto p = idx.find("2026-09-25 08:00:47");
    idx.replace(p, 19, acmr_accepted);
    pages[kAcmrIndex] = idx;
    pages[kAcmrEx99] = test::fixture("ex99-1.htm");
    pages["https://www.sec.gov/Archives/edgar/data/7654321/000110465926099999/0001104659-26-099999-index.htm"] =
        "<div class=\"infoHead\">Accepted</div>\n<div class=\"info\">2026-09-25 16:30:00</div>\n";
  }

  research::FetchFn fn() {
    return [this](const std::string& url) -> std::optional<std::string> {
      fetched.push_back(url);
      auto it = pages.find(url);
      if (it == pages.end()) return std::nullopt;
      return it->second;
    };
  }
};

}  // namespace

TEST(edgar_index_page_parsing) {
  const std::string idx = test::fixture("edgar_index.htm");
  const auto acc = research::parse_index_accepted(idx);
  CHECK(acc.has_value());
  if (acc)
    CHECK_EQ(*acc, timeutil::eastern_midnight_ns(2026, 9, 25) + static_cast<int64_t>(et(8, 0, 47)));
  CHECK_EQ(research::parse_index_items(idx), kItem101 | (1u << form8k_item_bit(9, 1)));
  CHECK(!research::parse_index_accepted("<div class=\"infoHead\">Filing Date</div><div class=\"info\">x</div>"));
  CHECK_EQ(research::filing_index_url("edgar/data/1234567/0001234567-26-000012.txt"), std::string(kAcmrIndex));
  CHECK_EQ(research::filing_index_url("garbage"), std::string());
  CHECK_EQ(research::master_index_url("2026-09-25"),
           std::string("https://www.sec.gov/Archives/edgar/daily-index/2026/QTR3/master.20260925.idx"));
  CHECK_EQ(research::master_index_url("2027-01-04"),
           std::string("https://www.sec.gov/Archives/edgar/daily-index/2027/QTR1/master.20270104.idx"));
}

TEST(edgar_history_builds_a_day) {
  const SymbolTable syms = test::fixture_symbols();
  FakeSec sec;
  std::vector<NewsEvent> ev;
  research::EdgarHistoryOptions o;
  const auto st = research::build_edgar_day("2026-09-25", syms, o, sec.fn(), ev);
  CHECK_EQ(st.index_rows, 5u);
  CHECK_EQ(st.candidates, 2u);  // ACMR 8-K + BRVO 424B5; Form 4, 10-Q and the OTC issuer are skipped
  CHECK_EQ(st.filings, 2u);
  CHECK_EQ(st.exhibits, 1u);
  CHECK_EQ(st.errors, 0u);
  CHECK_EQ(ev.size(), 3u);
  if (ev.size() != 3) return;
  const uint64_t mid = static_cast<uint64_t>(timeutil::eastern_midnight_ns(2026, 9, 25));
  // 1) the 8-K, visible 30 s after acceptance, issuer securities primary-first
  CHECK(ev[0].kind == EventKind::Filing);
  CHECK_EQ(ev[0].recv_ns, mid + et(9, 59, 30));
  CHECK_EQ(ev[0].published_ns, static_cast<int64_t>(mid + et(9, 59)));
  CHECK_EQ(ev[0].cik, 1234567u);
  CHECK_EQ(std::string(ev[0].form), std::string("8-K"));
  CHECK(ev[0].items_mask & kItem101);
  CHECK_EQ(std::string(ev[0].title.view()),
           std::string("8-K ACME ROBOTICS, INC.: Entry into a Material Definitive Agreement"));
  CHECK(ev[0].body.view().find("Item 1.01: Entry into a Material Definitive Agreement") != std::string_view::npos);
  CHECK(ev[0].n_tickers == 2 && ev[0].tickers[0] == syms.find("ACMR"));
  CHECK_EQ(std::string(ev[0].link.view()), std::string(kAcmrIndex));
  // 2) its EX-99.1 press release, 2 s later
  CHECK(ev[1].kind == EventKind::FilingDoc);
  CHECK_EQ(ev[1].recv_ns, mid + et(9, 59, 32));
  CHECK_EQ(ev[1].source, 1);
  CHECK_EQ(std::string(ev[1].title.view()),
           std::string("Acme Robotics Signs $20 Million Supply Agreement with Walmart"));
  CHECK(ev[1].tickers[0] == syms.find("ACMR"));
  CHECK_EQ(std::string(ev[1].link.view()), std::string(kAcmrEx99));
  // 3) the after-hours prospectus: dilution flag material, no exhibit fetch
  CHECK_EQ(std::string(ev[2].form), std::string("424B5"));
  CHECK_EQ(ev[2].recv_ns, mid + et(16, 30, 30));
  CHECK(ev[2].tickers[0] == syms.find("BRVO"));
  CHECK_EQ(ev[2].items_mask, 0u);
  CHECK_EQ(sec.fetched.size(), 4u);  // master + 2 index pages + 1 exhibit
}

TEST(edgar_history_options_and_failures) {
  const SymbolTable syms = test::fixture_symbols();
  {
    FakeSec sec;
    std::vector<NewsEvent> ev;
    research::EdgarHistoryOptions o;
    o.fetch_exhibits = false;
    o.dilution_forms = false;
    o.dissemination_delay_ms = 0;
    const auto st = research::build_edgar_day("2026-09-25", syms, o, sec.fn(), ev);
    CHECK_EQ(st.filings, 1u);
    CHECK_EQ(st.exhibits, 0u);
    CHECK(ev.size() == 1 && ev[0].recv_ns == static_cast<uint64_t>(ev[0].published_ns));
  }
  {
    // All issuers: the OTC 8-K becomes a candidate; its index page is missing -> counted, not fatal.
    FakeSec sec;
    std::vector<NewsEvent> ev;
    research::EdgarHistoryOptions o;
    o.listed_only = false;
    const auto st = research::build_edgar_day("2026-09-25", syms, o, sec.fn(), ev);
    CHECK_EQ(st.candidates, 3u);
    CHECK_EQ(st.errors, 1u);
    CHECK_EQ(st.filings, 2u);
  }
  {
    // No acceptance time: skipped rather than guessed.
    FakeSec sec;
    auto& idx = sec.pages[kAcmrIndex];
    idx.replace(idx.find(">Accepted<"), 10, ">Received<");
    std::vector<NewsEvent> ev;
    const auto st = research::build_edgar_day("2026-09-25", syms, {}, sec.fn(), ev);
    CHECK_EQ(st.missing_accepted, 1u);
    CHECK_EQ(st.filings, 1u);
  }
  {
    // Issuer missing from today's CIK map (renamed/delisted): the exhibit's own exchange tag resolves it.
    FakeSec sec;
    auto& master = sec.pages["https://www.sec.gov/Archives/edgar/daily-index/2026/QTR3/master.20260925.idx"];
    const auto p = master.find("1234567|ACME ROBOTICS, INC.|8-K|20260925|edgar/data/1234567/");
    master.replace(p, 7, "9999999");
    sec.pages["https://www.sec.gov/Archives/edgar/data/9999999/000123456726000012/0001234567-26-000012-index.htm"] =
        sec.pages[kAcmrIndex];
    std::vector<NewsEvent> ev;
    research::EdgarHistoryOptions o;
    o.listed_only = false;
    research::build_edgar_day("2026-09-25", syms, o, sec.fn(), ev);
    const NewsEvent* doc = nullptr;
    for (const auto& e : ev)
      if (e.kind == EventKind::FilingDoc) doc = &e;
    CHECK(doc != nullptr);
    if (doc) CHECK(doc->n_tickers >= 1 && doc->tickers[0] == syms.find("ACMR"));
  }
  {
    // No master index (holiday / not yet published).
    std::vector<NewsEvent> ev;
    const auto st = research::build_edgar_day(
        "2026-09-25", syms, {}, [](const std::string&) -> std::optional<std::string> { return std::nullopt; }, ev);
    CHECK_EQ(st.errors, 1u);
    CHECK(ev.empty());
  }
}

TEST(edgar_history_feeds_the_backtest) {
  // Rebuild the day from "EDGAR" instead of a recorded wire journal, then backtest it.
  Scenario sc;
  const SymbolTable syms = test::fixture_symbols();
  FakeSec sec;
  std::vector<NewsEvent> ev;
  research::build_edgar_day("2026-09-25", syms, {}, sec.fn(), ev);
  const std::string news = sc.root + "/news_edgar";
  fileutil::make_dirs(news + "/2026-09-25");
  Journal j(sc.root + "/unused", syms, nullptr, {"edgar_history", "edgar_history_ex99"});
  std::string jsonl;
  for (const auto& e : ev) {
    JournalRecord r{};
    r.type = JournalRecord::Type::News;
    r.news = e;
    r.news_sym = e.n_tickers ? e.tickers[0] : SymbolTable::kInvalid;
    jsonl += j.news_json(r) + "\n";
  }
  fileutil::write_file_atomic(news + "/2026-09-25/news.jsonl", jsonl);
  auto o = sc.opts("out_edgar");
  o.news_dir = news;
  const auto r = run_one(o);
  CHECK_EQ(r.news, 3u);
  CHECK_EQ(r.trades_closed, 1u);
  CHECK(r.pnl > 100);
  const auto data = research::load_journal(sc.root + "/out_edgar");
  CHECK(!data.trades.empty() && data.trades[0].ticker == "ACMR");
}
