// stok-bench: microbenchmarks for every stage on the hot path.
// Run on the target machine: ./build/stok-bench

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/histogram.hpp"
#include "core/log.hpp"
#include "core/seqlock.hpp"
#include "core/spsc_queue.hpp"
#include "engine/engine.hpp"
#include "engine/scorer.hpp"
#include "feeds/parsers.hpp"
#include "feeds/tickers.hpp"
#include "market/board.hpp"
#include "market/itch.hpp"
#include "market/itch_book.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

using namespace stok;

#ifndef STOK_SOURCE_DIR
#define STOK_SOURCE_DIR "."
#endif

namespace {

template <typename T>
inline void keep(const T& v) {
  asm volatile("" : : "g"(&v) : "memory");
}

std::string fixture(const std::string& n) {
  return fileutil::read_file(std::string(STOK_SOURCE_DIR) + "/tests/fixtures/" + n).value_or("");
}

SymbolTable symbols() {
  SymbolTable t;
  t.load_nasdaq_listed(fixture("nasdaqlisted.txt"));
  t.load_other_listed(fixture("otherlisted.txt"));
  t.load_sec_tickers(fixture("company_tickers_exchange.json"));
  // Pad to a realistic universe size so hash lookups aren't trivially cached.
  for (int i = 0; i < 12000; ++i) {
    SymbolInfo s;
    char tk[8];
    std::snprintf(tk, sizeof(tk), "Z%04d", i);
    s.ticker = tk;
    s.exchange = Exchange::NasdaqCM;
    t.add(std::move(s));
  }
  return t;
}

void row(const char* name, double ns_per_op, const char* unit_note) {
  if (ns_per_op >= 1e6) std::printf("  %-46s %10.2f ms   %s\n", name, ns_per_op / 1e6, unit_note);
  else if (ns_per_op >= 1e3) std::printf("  %-46s %10.2f us   %s\n", name, ns_per_op / 1e3, unit_note);
  else std::printf("  %-46s %10.1f ns   %s\n", name, ns_per_op, unit_note);
}

template <typename F>
double time_per_op(int iters, F&& f) {
  for (int i = 0; i < std::max(1, iters / 10); ++i) f();  // warm-up
  const uint64_t t0 = mono_ns();
  for (int i = 0; i < iters; ++i) f();
  return static_cast<double>(mono_ns() - t0) / iters;
}

}  // namespace

int main() {
  Logger::instance().set_level(LogLevel::Error);
  const SymbolTable syms = symbols();
  std::printf("stok-bench (%zu symbols)\n\n", syms.size());

  // ---------------- news path ----------------
  std::printf("news path\n");
  // A feed document of 100 items (typical RSS page size), entity-encoded
  // like GlobeNewswire's (no CDATA), with unique guids.
  std::string doc = "<rss><channel>";
  const std::string one = fixture("globenewswire.xml");
  const auto b = one.find("<item>"), e = one.find("</item>") + 7;
  const std::string item = one.substr(b, e - b);
  for (int i = 0; i < 100; ++i) {
    std::string copy = item;
    const auto g = copy.find("<guid isPermaLink=\"false\">");
    copy.insert(g + 26, std::to_string(i) + "-");
    doc += copy;
  }
  doc += "</channel></rss>";
  int n_items = 0;
  scan_feed(FeedKind::Rss, doc, [&](std::string_view, uint64_t) { ++n_items; });
  const double scan_ns = time_per_op(2000, [&] {
    uint64_t acc = 0;
    scan_feed(FeedKind::Rss, doc, [&](std::string_view, uint64_t id) { acc ^= id; });
    keep(acc);
  });
  char note[96];
  std::snprintf(note, sizeof(note), "%d items, %.0f KB (unchanged feed: ids only)", n_items, doc.size() / 1024.0);
  row("scan feed + hash item ids", scan_ns, note);
  ParseScratch scratch;
  std::vector<std::string> item_xml;
  scan_feed(FeedKind::Rss, doc, [&](std::string_view it, uint64_t) { item_xml.emplace_back(it); });
  NewsEvent ev;
  std::size_t k = 0;
  const double fill_ns = time_per_op(20000, [&] {
    ev.reset();
    fill_event(FeedKind::Rss, item_xml[k++ % item_xml.size()], syms, ev, scratch);
    keep(ev);
  });
  row("parse one new item -> NewsEvent (+tickers)", fill_ns, "entities, HTML strip, ticker resolution");
  const std::string edgar = fixture("edgar_current.xml");
  const double edgar_ns = time_per_op(20000, [&] {
    int c = 0;
    scan_feed(FeedKind::EdgarAtom, edgar, [&](std::string_view it, uint64_t) {
      ev.reset();
      fill_event(FeedKind::EdgarAtom, it, syms, ev, scratch);
      ++c;
    });
    keep(c);
  });
  row("parse EDGAR Atom (3 filings, full)", edgar_ns, "");
  TickerHit hits[16];
  const std::string pr_text = "Acme Robotics, Inc. (NASDAQ: ACMR) and Echo (TSXV: ECM) (OTCQB: ECMGF) today announced";
  row("ticker extraction (3 exchange tags)", time_per_op(200000, [&] { keep(extract_tickers(pr_text, syms, hits, 16)); }), "");

  Scorer scorer;
  std::string err;
  if (!scorer.load_rules_file(std::string(STOK_SOURCE_DIR) + "/config/rules.tsv", &err)) {
    std::fprintf(stderr, "rules: %s\n", err.c_str());
    return 1;
  }
  std::string body;
  while (body.size() < 3000)
    body += "Acme Robotics, Inc. (NASDAQ: ACMR), a maker of warehouse robots, today announced that it has signed a "
            "definitive three-year supply agreement with Walmart Inc. valued at $20 million. ";
  const std::string title = "Acme Robotics Signs $20 Million Supply Agreement with Walmart";
  const double score_ns = time_per_op(20000, [&] { keep(scorer.score_text(title, body)); });
  std::snprintf(note, sizeof(note), "%zu rules, %zu B text -> %.0f MB/s", scorer.rule_count(), body.size() + title.size(),
                (body.size() + title.size()) / score_ns * 1e3);
  row("score story (Aho-Corasick + amounts)", score_ns, note);

  // Engine end-to-end on one event (no market data).
  MarketBoard board(syms.size());
  FilingsHistory filings;
  SpscQueue<Signal> alerts(1 << 16);
  SpscQueue<JournalRecord> journal(1 << 12);
  Engine engine(EngineConfig{}, syms, &board, scorer, filings, &alerts, nullptr, &journal, nullptr, {"bench"});
  NewsEvent news;
  news.reset();
  fill_event(FeedKind::Rss, item_xml[0], syms, news, scratch);
  uint64_t id = 1;
  const double eng_ns = time_per_op(20000, [&] {
    news.id_hash = ++id;
    news.title_key = id;  // unique story each time
    news.recv_ns = wall_ns();
    engine.on_news(news, news.recv_ns);
    while (journal.front()) journal.pop();
    while (alerts.front()) alerts.pop();
  });
  row("engine: handle story -> signal + journal", eng_ns, "score, universe, dilution, watchlist, emit");

  // ---------------- market path ----------------
  std::printf("\nmarket path (ITCH 5.0)\n");
  // Two synthetic streams: prices clustered near a per-symbol mid (like real
  // order flow) and uniformly random prices (worst case for the ladders).
  auto make_stream = [](bool clustered, std::vector<uint8_t>& stream, std::vector<uint32_t>& offsets) {
    std::mt19937_64 rng(7);
    uint8_t m[64];
    auto push = [&](std::size_t n) {
      offsets.push_back(static_cast<uint32_t>(stream.size()));
      stream.insert(stream.end(), m, m + n);
    };
    const int kSyms = 5000;
    for (int s = 0; s < kSyms; ++s) {
      char tk[9];
      std::snprintf(tk, sizeof(tk), "Z%04d", s);
      push(itch::enc::stock_directory(m, static_cast<uint16_t>(s + 1), 0, tk));
    }
    uint64_t ref = 1;
    std::vector<uint64_t> live;
    for (int i = 0; i < 2'000'000; ++i) {
      const uint16_t loc = static_cast<uint16_t>(rng() % kSyms + 1);
      const uint64_t ts = 34'200'000'000'000ull + static_cast<uint64_t>(i) * 10'000;
      const int op = static_cast<int>(rng() % 10);
      if (op < 5 || live.size() < 1000) {
        const char side = (rng() & 1) ? 'B' : 'S';
        uint32_t px;
        if (clustered) {
          const uint32_t mid = 20000 + loc * 10;  // per-symbol mid
          const uint32_t off = static_cast<uint32_t>(rng() % 8) * static_cast<uint32_t>(rng() % 8) * 10;  // near touch
          px = side == 'B' ? mid - 10 - off : mid + 10 + off;
        } else {
          px = 10000 + static_cast<uint32_t>(rng() % 50000);
        }
        push(itch::enc::add(m, loc, ts, ref, side, 100 + rng() % 900, "Z0000", px));
        live.push_back(ref++);
      } else {
        const std::size_t pick = rng() % live.size();
        const uint64_t r = live[pick];
        if (op < 8) push(itch::enc::del(m, loc, ts, r));
        else push(itch::enc::executed(m, loc, ts, r, 100, i));
        live[pick] = live.back();
        live.pop_back();
      }
    }
  };
  MarketBoard mboard(syms.size());
  for (int scenario = 0; scenario < 3; ++scenario) {
    const bool clustered = scenario != 1;
    const bool quotes = scenario != 2;
    std::vector<uint8_t> stream;
    std::vector<uint32_t> offsets;
    make_stream(clustered, stream, offsets);
    MarketBoard board(syms.size());
    ItchBook book(syms, board, nullptr, nullptr, 1 << 22, quotes);
    const uint64_t t0 = mono_ns();
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      const std::size_t end = i + 1 < offsets.size() ? offsets[i + 1] : stream.size();
      itch::decode(stream.data() + offsets[i], end - offsets[i], book);
    }
    const double itch_ns = static_cast<double>(mono_ns() - t0) / static_cast<double>(offsets.size());
    std::snprintf(note, sizeof(note), "%.1fM msgs, %.1f M msg/s, %zu live orders", offsets.size() / 1e6,
                  1e3 / itch_ns, book.live_orders());
    const char* names[] = {"decode + orders + top of book (clustered px)", "decode + orders + top of book (random px)",
                           "decode + orders, no quotes"};
    row(names[scenario], itch_ns, note);
  }
  row("seqlock snapshot (hot fields)", time_per_op(1'000'000, [&] { keep(mboard.hot(syms.find("Z0001"))); }), "engine read of one symbol");

  // ---------------- inter-thread ----------------
  std::printf("\ninter-thread\n");
  {
    SpscQueue<uint64_t> a(1024), bq(1024);
    std::atomic<bool> stop{false};
    std::thread echo([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        if (uint64_t* v = a.front()) {
          const uint64_t x = *v;
          a.pop();
          while (!bq.try_push(x)) cpu_relax();
        } else {
          cpu_relax();
        }
      }
    });
    LatencyHistogram rt;
    for (int i = 0; i < 200000; ++i) {
      const uint64_t s = mono_ns();
      while (!a.try_push(s)) cpu_relax();
      uint64_t* v;
      while (!(v = bq.front())) cpu_relax();
      bq.pop();
      rt.record(mono_ns() - s);
    }
    stop = true;
    echo.join();
    std::snprintf(note, sizeof(note), "p50 %llu ns, p99 %llu ns (busy-polling both sides)",
                  static_cast<unsigned long long>(rt.percentile(50)), static_cast<unsigned long long>(rt.percentile(99)));
    row("SPSC ring round trip (2 hops)", static_cast<double>(rt.percentile(50)), note);
  }
  std::printf("\nNote: network round trips to the sources (typically 5-80 ms) dominate end-to-end latency;\n"
              "everything above is what happens after the bytes arrive.\n");
  Logger::instance().stop();
  return 0;
}
