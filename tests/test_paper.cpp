#include <cmath>
#include <vector>

#include "check.hpp"
#include "engine/paper.hpp"
#include "fixture_symbols.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

constexpr uint64_t kSec = 1'000'000'000ull;

struct PaperRig {
  SymbolTable syms = test::fixture_symbols();
  MarketBoard board{syms.size()};
  PaperConfig cfg;
  std::unique_ptr<PaperTrader> pt;
  std::vector<PaperTrade> events;
  uint32_t acmr = 0;
  uint64_t t0 = 0;  // 10:00 ET on 2026-09-25

  explicit PaperRig(PaperConfig c = {}) : cfg(c) {
    board.set_session_date(2026, 9, 25);
    acmr = syms.find("ACMR");
    t0 = static_cast<uint64_t>(board.midnight_ns()) + 10ull * 3600 * kSec;
    cfg.enabled = true;
    pt = std::make_unique<PaperTrader>(cfg, syms, board);
  }
  void quote(double bid, double ask) {
    board.on_quote(acmr, static_cast<int32_t>(std::lround(bid * 1e4)), 1000, static_cast<int32_t>(std::lround(ask * 1e4)), 1000,
                   t0 - static_cast<uint64_t>(board.midnight_ns()));
  }
  void signal(Tier tier, double ask, uint64_t at) {
    Signal s{};
    s.tier = tier;
    s.sym = acmr;
    s.score = 80;
    s.ask = ask;
    s.price = ask;
    s.catalyst = Catalyst::Contract;
    std::snprintf(s.ticker, sizeof(s.ticker), "ACMR");
    pt->on_signal(s, at);
  }
  void step(uint64_t at) {
    pt->step(at, [&](const PaperTrade& t) { events.push_back(t); });
  }
};

PaperConfig base_cfg() {
  PaperConfig c;
  c.latency_ms = 0;
  c.slippage_bps = 0;
  c.position_usd = 1000;
  c.flat_by_minute = 24 * 60;  // off unless a test sets it
  return c;
}

}  // namespace

TEST(paper_take_profit) {
  PaperRig r(base_cfg());
  r.quote(2.39, 2.41);
  r.signal(Tier::High, 2.41, r.t0);
  r.step(r.t0);
  CHECK_EQ(r.events.size(), 1u);
  if (r.events.empty()) return;
  CHECK(!r.events[0].closed);
  CHECK_NEAR(r.events[0].entry_px, 2.41, 1e-9);
  CHECK_EQ(r.events[0].shares, 414.0);
  CHECK(r.events[0].quote_entry);
  r.quote(2.95, 2.97);  // bid +22.4%
  r.step(r.t0 + 60 * kSec);
  CHECK_EQ(r.events.size(), 2u);
  if (r.events.size() < 2) return;
  const PaperTrade& t = r.events[1];
  CHECK(t.closed);
  CHECK_EQ(std::string(t.exit_reason), std::string("target"));
  CHECK_NEAR(t.exit_px, 2.95, 1e-9);
  CHECK_NEAR(t.pnl_usd, (2.95 - 2.41) * 414, 1e-6);
  CHECK_EQ(r.pt->stats().wins, 1u);
  CHECK_EQ(r.pt->open_positions(), 0u);
}

TEST(paper_stop_gaps_through_while_halted) {
  PaperRig r(base_cfg());
  r.quote(1.99, 2.00);
  r.signal(Tier::Alert, 2.00, r.t0);
  r.step(r.t0);
  r.board.on_trading_action(r.acmr, 'H', "LUDP");
  r.quote(1.50, 1.52);
  r.step(r.t0 + 10 * kSec);
  CHECK_EQ(r.events.size(), 1u);  // can't exit while halted
  r.board.on_trading_action(r.acmr, 'T', "");
  r.step(r.t0 + 300 * kSec);
  CHECK_EQ(r.events.size(), 2u);
  if (r.events.size() < 2) return;
  CHECK_EQ(std::string(r.events[1].exit_reason), std::string("stop"));
  CHECK_NEAR(r.events[1].pnl_pct, -25.0, 1e-6);  // the gap, not the 8% stop, is the real loss
  CHECK_NEAR(r.pt->stats().max_drawdown, -r.events[1].pnl_usd, 1e-6);
}

TEST(paper_trailing_and_time_stops) {
  PaperRig r(base_cfg());
  r.quote(1.99, 2.00);
  r.signal(Tier::High, 2.00, r.t0);
  r.step(r.t0);
  r.quote(2.30, 2.31);  // +15%: trail armed (activates at +10%)
  r.step(r.t0 + 30 * kSec);
  r.quote(2.12, 2.13);  // +6% <= 15% - 8%
  r.step(r.t0 + 60 * kSec);
  CHECK_EQ(r.events.size(), 2u);
  if (r.events.size() == 2) {
    CHECK_EQ(std::string(r.events[1].exit_reason), std::string("trail"));
    CHECK_NEAR(r.events[1].mfe_pct, 15.0, 1e-6);
  }
  PaperRig q(base_cfg());
  q.quote(1.99, 2.00);
  q.signal(Tier::High, 2.00, q.t0);
  q.step(q.t0);
  q.quote(2.05, 2.06);
  q.step(q.t0 + 29 * 60 * kSec);
  CHECK_EQ(q.events.size(), 1u);
  q.step(q.t0 + 31 * 60 * kSec);
  CHECK_EQ(q.events.size(), 2u);
  if (q.events.size() == 2) CHECK_EQ(std::string(q.events[1].exit_reason), std::string("time"));
}

TEST(paper_flat_by_close) {
  PaperConfig c = base_cfg();
  c.flat_by_minute = 15 * 60 + 55;
  c.max_hold_min = 1000;
  PaperRig r(c);
  const uint64_t t1500 = static_cast<uint64_t>(r.board.midnight_ns()) + 15ull * 3600 * kSec;
  r.quote(1.99, 2.00);
  r.signal(Tier::High, 2.00, t1500);
  r.step(t1500);
  r.step(t1500 + 54 * 60 * kSec);
  CHECK_EQ(r.events.size(), 1u);
  r.step(t1500 + 56 * 60 * kSec);  // 15:56 ET
  CHECK_EQ(r.events.size(), 2u);
  if (r.events.size() == 2) CHECK_EQ(std::string(r.events[1].exit_reason), std::string("flat"));
}

TEST(paper_latency_chase_and_slippage) {
  PaperConfig c = base_cfg();
  c.latency_ms = 500;
  c.slippage_bps = 50;
  PaperRig r(c);
  r.quote(1.99, 2.00);
  r.signal(Tier::High, 2.00, r.t0);
  r.step(r.t0 + 100'000'000);  // 100 ms: order hasn't arrived yet
  CHECK(r.events.empty());
  r.step(r.t0 + 600'000'000);
  CHECK_EQ(r.events.size(), 1u);
  if (!r.events.empty()) CHECK_NEAR(r.events[0].entry_px, 2.00 * 1.005, 1e-9);

  PaperRig ch(c);
  ch.quote(1.99, 2.00);
  ch.signal(Tier::High, 2.00, ch.t0);
  ch.quote(2.19, 2.20);  // ran 10% during our latency
  ch.step(ch.t0 + kSec);
  CHECK(ch.events.empty());
  CHECK_EQ(ch.pt->stats().cancelled_chase, 1u);
  CHECK_EQ(ch.pt->open_positions(), 0u);
}

TEST(paper_risk_limits) {
  PaperConfig c = base_cfg();
  c.max_daily_loss_usd = 100;
  c.max_open = 1;
  PaperRig r(c);
  r.quote(1.99, 2.00);
  r.signal(Tier::Alert, 2.00, r.t0);
  r.signal(Tier::High, 2.00, r.t0);  // ALERT -> HIGH upgrade: same ticker, no second position
  CHECK_EQ(r.pt->stats().rejected_dup, 1u);
  r.step(r.t0);
  r.quote(1.70, 1.71);  // -15%: stop, loses ~$150
  r.step(r.t0 + 10 * kSec);
  CHECK_EQ(r.pt->stats().closed, 1u);
  CHECK(r.pt->stats().day_realized < -100);
  r.signal(Tier::High, 1.71, r.t0 + 20 * kSec);
  CHECK_EQ(r.pt->stats().rejected_risk, 1u);  // daily loss limit reached
  CHECK_EQ(r.pt->open_positions(), 0u);
}
