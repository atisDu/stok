#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "check.hpp"
#include "engine/engine.hpp"
#include "fixture_symbols.hpp"
#include "market/baseline.hpp"
#include "market/itch.hpp"
#include "market/itch_book.hpp"
#include "market/sources.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

using namespace stok;
namespace enc = stok::itch::enc;

namespace {

constexpr uint64_t kMin = 60'000'000'000ull;
uint64_t at(int h, int m, int s = 0) { return (static_cast<uint64_t>(h) * 60 + m) * kMin + static_cast<uint64_t>(s) * 1'000'000'000ull; }

struct Stream {
  std::vector<std::vector<uint8_t>> msgs;
  template <typename F>
  void add(F&& f) {
    std::vector<uint8_t> m(64);
    m.resize(f(m.data()));
    msgs.push_back(std::move(m));
  }
};

Stream session() {
  Stream s;
  s.add([](uint8_t* p) { return enc::system_event(p, at(3, 0), 'O'); });
  s.add([](uint8_t* p) { return enc::stock_directory(p, 1, at(3, 0), "ACMR"); });
  s.add([](uint8_t* p) { return enc::stock_directory(p, 2, at(3, 0), "DLTX", 'G'); });
  s.add([](uint8_t* p) { return enc::trading_action(p, 1, at(3, 1), "ACMR", 'T', ""); });   // start-of-day burst
  s.add([](uint8_t* p) { return enc::add(p, 1, at(8, 0), 1, 'B', 1000, "ACMR", 20000); });
  s.add([](uint8_t* p) { return enc::add(p, 1, at(8, 0), 2, 'S', 500, "ACMR", 21000); });
  s.add([](uint8_t* p) { return enc::executed(p, 1, at(8, 1), 1, 300, 11); });              // 300 @ 2.00 (pre-market)
  s.add([](uint8_t* p) { return enc::executed_price(p, 1, at(8, 2), 2, 200, true, 20500); }); // 200 @ 2.05
  s.add([](uint8_t* p) { return enc::executed_price(p, 1, at(8, 2), 2, 100, false, 20500); });// non-printable
  s.add([](uint8_t* p) { return enc::cancel(p, 1, at(8, 3), 1, 200); });                    // order 1: 500 left
  s.add([](uint8_t* p) { return enc::replace(p, 1, at(8, 4), 1, 3, 400, 22000); });         // -> order 3
  s.add([](uint8_t* p) { return enc::cross(p, 1, at(9, 30), 5000, "ACMR", 22500, 'O'); });  // opening cross
  s.add([](uint8_t* p) { return enc::executed(p, 1, at(9, 31), 3, 400, 12); });             // 400 @ 2.20
  s.add([](uint8_t* p) { return enc::del(p, 1, at(9, 32), 2); });
  s.add([](uint8_t* p) { return enc::trade(p, 1, at(9, 33), 'B', 1000, "ACMR", 23000); });  // hidden 1000 @ 2.30
  s.add([](uint8_t* p) { return enc::trading_action(p, 1, at(9, 34), "ACMR", 'H', "LUDP"); });
  s.add([](uint8_t* p) { return enc::trading_action(p, 1, at(9, 39), "ACMR", 'T', ""); });
  s.add([](uint8_t* p) { return enc::trade(p, 2, at(10, 0), 'B', 100, "DLTX", 45000); });
  return s;
}

}  // namespace

TEST(itch_book_prices_executions) {
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  SpscQueue<MarketEvent> evq(64);
  ItchBook book(syms, board, &evq, nullptr, 1024);
  for (const auto& m : session().msgs) CHECK(itch::decode(m.data(), m.size(), book));
  const uint32_t acmr = syms.find("ACMR");
  const MarketHot h = board.hot(acmr);
  CHECK_EQ(h.day_volume, 300u + 200u + 5000u + 400u + 1000u);
  CHECK_EQ(h.last_px, 23000);
  CHECK_EQ(h.open_px, 22500);      // opening cross sets the official open
  CHECK_EQ(h.pm_high_px, 20500);
  CHECK_EQ(h.high_px, 23000);
  CHECK_EQ(h.low_px, 20000);
  CHECK_EQ(h.trading_state, 'T');
  const double vwap = (300 * 2.0 + 200 * 2.05 + 5000 * 2.25 + 400 * 2.2 + 1000 * 2.3) / 6900.0;
  CHECK_NEAR(h.vwap(), vwap, 1e-9);
  CHECK_EQ(book.live_orders(), 0u);  // everything executed, cancelled or deleted
  CHECK_EQ(h.last_trade_ns, timeutil::eastern_midnight_ns(2026, 9, 25) + static_cast<int64_t>(at(9, 33)));
  // Minute ring: the 09:33 bar holds the hidden trade.
  const MarketState full = board.full(acmr);
  CHECK_EQ(full.volume_since_minute(9 * 60 + 33), 1000u);
  CHECK_EQ(full.price_at_minute(8 * 60 + 30), 20500);
  // Engine sees: system event, halt, resume (not the start-of-day 'T').
  int actions = 0, systems = 0;
  char states[4] = {};
  while (MarketEvent* e = evq.front()) {
    if (e->type == MarketEvent::Type::TradingAction) {
      if (actions < 4) states[actions] = e->state;
      ++actions;
    } else {
      ++systems;
    }
    evq.pop();
  }
  CHECK_EQ(actions, 2);
  CHECK_EQ(states[0], 'H');
  CHECK_EQ(states[1], 'T');
  CHECK_EQ(systems, 1);
  CHECK_EQ(board.hot(syms.find("DLTX")).last_px, 45000);
}

TEST(moldudp64_sequencing) {
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  ItchBook book(syms, board, nullptr, nullptr, 64);
  const auto msgs = session().msgs;
  auto packet = [&](uint64_t seq, std::size_t first, std::size_t count) {
    std::vector<uint8_t> p(20);
    std::memcpy(p.data(), "SESSION001", 10);
    enc::put64(p.data() + 10, seq);
    enc::put16(p.data() + 18, static_cast<uint16_t>(count));
    for (std::size_t i = first; i < first + count; ++i) {
      uint8_t len[2];
      enc::put16(len, static_cast<uint16_t>(msgs[i].size()));
      p.insert(p.end(), len, len + 2);
      p.insert(p.end(), msgs[i].begin(), msgs[i].end());
    }
    return p;
  };
  MoldUdp64Receiver rx;
  auto p1 = packet(1, 0, 3);
  auto p2 = packet(4, 3, 2);
  auto p_gap = packet(8, 5, 1);    // 6 and 7 missing
  auto p_dup = packet(1, 0, 3);    // replay of the first packet
  auto p_overlap = packet(8, 5, 2);  // seq 8 again (already seen) + seq 9 (new)
  rx.on_packet(p1.data(), p1.size(), book);
  rx.on_packet(p2.data(), p2.size(), book);
  rx.on_packet(p_gap.data(), p_gap.size(), book);
  rx.on_packet(p_dup.data(), p_dup.size(), book);
  rx.on_packet(p_overlap.data(), p_overlap.size(), book);
  const auto& st = rx.stats();
  CHECK_EQ(st.packets, 5u);
  CHECK_EQ(st.gaps, 1u);
  CHECK_EQ(st.gap_messages, 2u);
  CHECK_EQ(st.dups, 1u);
  CHECK_EQ(st.messages, 3u + 2u + 1u + 1u);  // overlap: only the new message decoded
  CHECK_EQ(book.sym_for_locate(1), syms.find("ACMR"));
}

TEST(itch_file_reader_gz_roundtrip) {
  const std::string dir = "build/test_tmp";
  fileutil::make_dirs(dir);
  const std::string path = dir + "/09252026.NASDAQ_ITCH50.gz";
  gzFile gz = gzopen(path.c_str(), "wb");
  const auto msgs = session().msgs;
  for (int rep = 0; rep < 1000; ++rep) {
    for (const auto& m : msgs) {
      uint8_t len[2];
      enc::put16(len, static_cast<uint16_t>(m.size()));
      gzwrite(gz, len, 2);
      gzwrite(gz, m.data(), static_cast<unsigned>(m.size()));
    }
  }
  gzclose(gz);
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  ItchBook book(syms, board, nullptr, nullptr, 1024);
  ItchFileReader r;
  std::string err;
  CHECK(r.open(path, &err));
  std::atomic<bool> stop{false};
  while (r.run(book, stop, 0.0, 777)) {
  }
  CHECK_EQ(r.stats().messages, 1000u * msgs.size());
  CHECK_EQ(r.stats().bad, 0u);
  CHECK_EQ(board.hot(syms.find("ACMR")).day_volume, 1000u * 6900u);
  CHECK_EQ(std::string("2026-09-25"), stok::timeutil::eastern_date_str(stok::timeutil::eastern_midnight_ns(2026, 9, 25) + 1));
}

TEST(bridge_text_protocol) {
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  BridgeReceiver b;
  struct Ctx {
    int states = 0;
  } ctx;
  const int64_t ts = timeutil::eastern_midnight_ns(2026, 9, 25) + static_cast<int64_t>(at(9, 45));
  const std::string msg = "T ACMR 2.5 1000 " + std::to_string(ts) + "\nH ACMR H LUDP\nT NOPE 1 1\nX what\nT ACMR -1 5\n";
  b.handle(msg, syms, board, [](void* c, uint32_t, char, const char*, int64_t) { ++static_cast<Ctx*>(c)->states; }, &ctx);
  const MarketHot h = board.hot(syms.find("ACMR"));
  CHECK_EQ(h.last_px, 25000);
  CHECK_EQ(h.day_volume, 1000u);
  CHECK_EQ(h.trading_state, 'H');
  CHECK_EQ(std::string(h.halt_reason), std::string("LUDP"));
  CHECK_EQ(ctx.states, 1);
  CHECK_EQ(b.stats().unknown_symbol, 1u);
  CHECK_EQ(b.stats().bad, 2u);
}

TEST(baseline_store_append_and_rebuild) {
  const std::string dir = "build/test_tmp/baseline";
  std::filesystem::remove_all(dir);
  const SymbolTable syms = test::fixture_symbols();
  for (int day = 0; day < 3; ++day) {
    MarketBoard board(syms.size());
    board.set_session_date(2026, 9, 22 + day);
    board.on_trade(syms.find("ACMR"), 20000 + day * 1000, 100000 * static_cast<uint64_t>(day + 1), at(10, 0));
    board.on_cross(syms.find("ACMR"), 21000 + day * 1000, 0, at(16, 0), 'C');
    BaselineStore store(dir);
    CHECK_EQ(store.append_session("2026-09-2" + std::to_string(2 + day), board, syms), 1u);
  }
  BaselineStore store(dir);
  CHECK(store.rebuild(20));
  SymbolTable t = test::fixture_symbols();
  auto txt = fileutil::read_file(store.baseline_path());
  CHECK(txt.has_value());
  if (!txt) return;
  CHECK_EQ(t.load_baseline(*txt), 1u);
  CHECK_NEAR(t[t.find("ACMR")].adv20, 200000.0, 1e-6);   // mean of 100k, 200k, 300k
  CHECK_NEAR(t[t.find("ACMR")].prev_close, 2.3, 1e-9);   // latest official close
  CHECK_EQ(t[t.find("ACMR")].adv_days, 3u);
}

TEST(expected_volume_curve_is_monotonic) {
  double prev = -1;
  for (int m = 0; m < 24 * 60; ++m) {
    const double f = expected_volume_fraction(m);
    CHECK(f >= prev - 1e-12);
    prev = f;
  }
  CHECK_NEAR(expected_volume_fraction(9 * 60 + 30), 0.04, 1e-9);
  CHECK_NEAR(expected_volume_fraction(16 * 60), 0.96, 1e-9);
  CHECK_NEAR(expected_volume_fraction(23 * 60), 1.0, 1e-9);
}
