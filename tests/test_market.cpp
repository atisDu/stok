#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <vector>

#include "check.hpp"
#include "engine/engine.hpp"
#include "fixture_symbols.hpp"
#include "market/baseline.hpp"
#include "market/itch.hpp"
#include "market/itch_book.hpp"
#include "market/level_book.hpp"
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

TEST(level_ladder_matches_reference_map) {
  std::mt19937_64 rng(3);
  for (int side = 0; side < 2; ++side) {
    const bool is_bid = side == 0;
    SideLadder lad(is_bid);
    std::map<uint32_t, uint64_t> ref;
    for (int i = 0; i < 200000; ++i) {
      const uint32_t px = 10000 + static_cast<uint32_t>(rng() % 300) * 10;
      const uint64_t q = 1 + rng() % 500;
      if (rng() % 3 != 0) {
        lad.add(px, q);
        ref[px] += q;
      } else {
        auto it = ref.find(px);
        lad.remove(px, q);
        if (it != ref.end()) {
          if (q >= it->second) ref.erase(it);
          else it->second -= q;
        }
      }
      if ((i & 1023) == 0) {
        CHECK_EQ(lad.depth(), ref.size());
        if (!ref.empty()) {
          const auto best = is_bid ? *ref.rbegin() : *ref.begin();
          CHECK_EQ(lad.best_price(), best.first);
          CHECK_EQ(lad.best_qty(), best.second);
        }
      }
    }
  }
}

TEST(itch_book_top_of_book) {
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  ItchBook book(syms, board, nullptr, nullptr, 1024);
  std::vector<uint8_t> m(64);
  auto run = [&](std::size_t n) { itch::decode(m.data(), n, book); };
  const uint32_t acmr = syms.find("ACMR");
  run(enc::stock_directory(m.data(), 1, at(3, 0), "ACMR"));
  run(enc::add(m.data(), 1, at(8, 0), 1, 'B', 500, "ACMR", 19900));
  run(enc::add(m.data(), 1, at(8, 0), 2, 'B', 300, "ACMR", 20000));
  run(enc::add(m.data(), 1, at(8, 0), 3, 'B', 200, "ACMR", 20000));
  run(enc::add(m.data(), 1, at(8, 0), 4, 'S', 400, "ACMR", 20200));
  run(enc::add(m.data(), 1, at(8, 0), 5, 'S', 100, "ACMR", 20500));
  MarketHot h = board.hot(acmr);
  CHECK_EQ(h.bid_px, 20000);
  CHECK_EQ(h.bid_sz, 500u);  // two orders at 2.00
  CHECK_EQ(h.ask_px, 20200);
  CHECK_EQ(h.ask_sz, 400u);
  CHECK(h.quote_valid());
  CHECK_NEAR(h.spread_pct(), 200.0 * 200 / 40200.0, 1e-9);
  run(enc::executed(m.data(), 1, at(8, 1), 4, 400, 1));  // ask level 2.02 swept
  h = board.hot(acmr);
  CHECK_EQ(h.ask_px, 20500);
  CHECK_EQ(h.ask_sz, 100u);
  run(enc::cancel(m.data(), 1, at(8, 2), 2, 100));       // 2.00 bid: 400 left
  run(enc::del(m.data(), 1, at(8, 2), 3));               // 2.00 bid: 200 left
  h = board.hot(acmr);
  CHECK_EQ(h.bid_px, 20000);
  CHECK_EQ(h.bid_sz, 200u);
  run(enc::replace(m.data(), 1, at(8, 3), 2, 6, 700, 20100));  // bid moves up to 2.01
  h = board.hot(acmr);
  CHECK_EQ(h.bid_px, 20100);
  CHECK_EQ(h.bid_sz, 700u);
  run(enc::executed_price(m.data(), 1, at(8, 4), 6, 700, true, 20100));
  h = board.hot(acmr);
  CHECK_EQ(h.bid_px, 19900);  // back to the 1.99 level
  CHECK_EQ(h.bid_sz, 500u);
  CHECK(book.book(acmr) != nullptr);
  CHECK_EQ(book.book(acmr)->bids.depth(), 1u);
}

TEST(bridge_quote_lines) {
  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  BridgeReceiver b;
  b.handle("Q ACMR 2.40 1200 2.45 800\nQ ACMR 2.40 1200\n", syms, board, nullptr, nullptr);
  const MarketHot h = board.hot(syms.find("ACMR"));
  CHECK_EQ(h.bid_px, 24000);
  CHECK_EQ(h.ask_px, 24500);
  CHECK_EQ(h.bid_sz, 1200u);
  CHECK_EQ(h.ask_sz, 800u);
  CHECK_EQ(b.stats().quotes, 1u);
  CHECK_EQ(b.stats().bad, 1u);
}

namespace {

std::vector<std::vector<uint8_t>> mold_packets(const std::vector<std::vector<uint8_t>>& msgs,
                                               const std::vector<std::pair<std::size_t, std::size_t>>& spans) {
  std::vector<std::vector<uint8_t>> out;
  for (auto [first, count] : spans) {
    std::vector<uint8_t> p(20);
    std::memcpy(p.data(), "SESSION001", 10);
    enc::put64(p.data() + 10, first + 1);  // MoldUDP64 sequence numbers start at 1
    enc::put16(p.data() + 18, static_cast<uint16_t>(count));
    for (std::size_t i = first; i < first + count; ++i) {
      uint8_t len[2];
      enc::put16(len, static_cast<uint16_t>(msgs[i].size()));
      p.insert(p.end(), len, len + 2);
      p.insert(p.end(), msgs[i].begin(), msgs[i].end());
    }
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace

TEST(moldudp64_gap_recovery_replays_in_order) {
  const SymbolTable syms = test::fixture_symbols();
  const auto msgs = session().msgs;  // 18 messages
  // Reference: everything in order.
  MarketBoard ref_board(syms.size());
  ref_board.set_session_date(2026, 9, 25);
  ItchBook ref_book(syms, ref_board, nullptr, nullptr, 64);
  for (const auto& m : msgs) itch::decode(m.data(), m.size(), ref_book);

  const auto pk = mold_packets(msgs, {{0, 4}, {4, 3}, {7, 4}, {11, 4}, {15, 3}});
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  ItchBook book(syms, board, nullptr, nullptr, 64);
  MoldUdp64Receiver rx;
  std::vector<std::pair<uint64_t, uint16_t>> requests;
  rx.enable_recovery([&](const char*, uint64_t seq, uint16_t count) { requests.emplace_back(seq, count); }, 250, 50);
  uint64_t now = 1'000'000'000;
  rx.on_packet(pk[0].data(), pk[0].size(), book, now);  // seq 1-4
  rx.on_packet(pk[2].data(), pk[2].size(), book, now);  // seq 8-11: gap 5-7 -> request
  rx.on_packet(pk[4].data(), pk[4].size(), book, now);  // seq 16-18: buffered
  CHECK(rx.recovering());
  CHECK_EQ(requests.size(), 1u);
  if (!requests.empty()) {
    CHECK_EQ(requests[0].first, 5u);
    CHECK_EQ(requests[0].second, 3);
  }
  CHECK_EQ(rx.stats().messages, 4u);  // nothing past the gap applied yet
  rx.on_packet(pk[1].data(), pk[1].size(), book, now + 1'000'000);  // retransmission of 5-7
  CHECK(rx.recovering());             // 12-15 still missing -> next request
  CHECK_EQ(requests.size(), 2u);
  if (requests.size() == 2) {
    CHECK_EQ(requests[1].first, 12u);
    CHECK_EQ(requests[1].second, 4);
  }
  rx.on_packet(pk[3].data(), pk[3].size(), book, now + 2'000'000);  // 12-15
  CHECK(!rx.recovering());
  CHECK_EQ(rx.expected_seq(), 19u);
  CHECK_EQ(rx.stats().messages, 18u);
  CHECK_EQ(rx.stats().recovered, 1u);
  const uint32_t acmr = syms.find("ACMR");
  CHECK_EQ(board.hot(acmr).day_volume, ref_board.hot(acmr).day_volume);
  CHECK_EQ(board.hot(acmr).last_px, ref_board.hot(acmr).last_px);
  CHECK_NEAR(board.hot(acmr).vwap(), ref_board.hot(acmr).vwap(), 1e-12);
  CHECK_EQ(book.stats().unknown_ref, 0u);
}

TEST(moldudp64_gap_timeout_gives_up) {
  const SymbolTable syms = test::fixture_symbols();
  const auto msgs = session().msgs;
  const auto pk = mold_packets(msgs, {{0, 4}, {4, 3}, {7, 4}});
  MarketBoard board(syms.size());
  ItchBook book(syms, board, nullptr, nullptr, 64);
  MoldUdp64Receiver rx;
  int requests = 0;
  rx.enable_recovery([&](const char*, uint64_t, uint16_t) { ++requests; }, 100, 20);
  const uint64_t t = 5'000'000'000;
  rx.on_packet(pk[0].data(), pk[0].size(), book, t);
  rx.on_packet(pk[2].data(), pk[2].size(), book, t);
  rx.tick(book, t + 30'000'000);   // retry
  rx.tick(book, t + 60'000'000);   // retry
  CHECK(requests >= 2);
  rx.tick(book, t + 150'000'000);  // timeout: skip 5-7, apply 8-11
  CHECK(!rx.recovering());
  CHECK_EQ(rx.stats().gap_timeouts, 1u);
  CHECK_EQ(rx.stats().gap_messages, 3u);
  CHECK_EQ(rx.stats().messages, 8u);
  CHECK_EQ(rx.expected_seq(), 12u);
}

TEST(moldudp64_socket_rerequest_roundtrip) {
  // Fake "re-request server" on loopback.
  const int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = 0;
  CHECK(::bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
  socklen_t sl = sizeof(sa);
  getsockname(srv, reinterpret_cast<sockaddr*>(&sa), &sl);
  const uint16_t srv_port = ntohs(sa.sin_port);

  MoldUdp64Receiver rx;
  MoldUdp64Receiver::Options o;
  o.port = 0;                  // ephemeral, unicast (no multicast group)
  o.busy_poll = false;
  o.rerequest = "127.0.0.1:" + std::to_string(srv_port);
  std::string err;
  CHECK(rx.open(o, &err));
  sockaddr_in rx_addr{};
  rx_addr.sin_family = AF_INET;
  rx_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  rx_addr.sin_port = htons(rx.local_port());

  const SymbolTable syms = test::fixture_symbols();
  MarketBoard board(syms.size());
  board.set_session_date(2026, 9, 25);
  ItchBook book(syms, board, nullptr, nullptr, 64);
  const auto msgs = session().msgs;
  const auto pk = mold_packets(msgs, {{0, 6}, {6, 6}, {12, 6}});
  auto send = [&](const std::vector<uint8_t>& p) {
    ::sendto(srv, p.data(), p.size(), 0, reinterpret_cast<const sockaddr*>(&rx_addr), sizeof(rx_addr));
  };
  send(pk[0]);
  send(pk[2]);  // packet 2 "lost"
  for (int i = 0; i < 20 && rx.stats().packets < 2; ++i) rx.poll(book, 20);
  CHECK(rx.recovering());
  // Server receives the request and answers with the missing packet.
  uint8_t req[64];
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  pollfd pf{srv, POLLIN, 0};
  CHECK(::poll(&pf, 1, 1000) == 1);
  const ssize_t rn = ::recvfrom(srv, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK_EQ(rn, 20);
  CHECK(std::memcmp(req, "SESSION001", 10) == 0);
  CHECK_EQ(itch::be64(req + 10), 7u);
  CHECK_EQ(itch::be16(req + 18), 6);
  ::sendto(srv, pk[1].data(), pk[1].size(), 0, reinterpret_cast<const sockaddr*>(&from), fl);
  for (int i = 0; i < 20 && rx.recovering(); ++i) rx.poll(book, 20);
  CHECK(!rx.recovering());
  CHECK_EQ(rx.stats().messages, 18u);
  CHECK_EQ(board.hot(syms.find("ACMR")).day_volume, 6900u);
  ::close(srv);
}
