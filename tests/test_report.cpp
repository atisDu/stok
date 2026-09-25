#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "research/report.hpp"

using namespace stok;
using namespace stok::research;

namespace {

void write(const std::string& path, const std::string& text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path) << text;
}

std::string trade(const char* tier, const char* reason, double pnl, uint64_t exit_ns) {
  char b[512];
  std::snprintf(b, sizeof(b),
                "{\"event\":\"close\",\"ticker\":\"ACMR\",\"tier\":\"%s\",\"catalyst\":\"contract\",\"score\":80,"
                "\"exit_ns\":%llu,\"exit_reason\":\"%s\",\"pnl_usd\":%.2f,\"pnl_pct\":%.2f,\"mfe_pct\":1,\"mae_pct\":-1,"
                "\"hold_s\":60}\n",
                tier, static_cast<unsigned long long>(exit_ns), reason, pnl, pnl / 10.0);
  return b;
}

}  // namespace

TEST(report_trade_stats_math) {
  std::vector<TradeRow> t;
  const double pnls[] = {100, -50, 200, -50, -100, 30};
  uint64_t ts = 1;
  for (double p : pnls) {
    TradeRow r;
    r.day = "2026-09-25";
    r.pnl_usd = p;
    r.pnl_pct = p / 10;
    r.exit_ns = ts++;
    t.push_back(r);
  }
  t.back().day = "2026-10-05";
  const TradeStats s = trade_stats(t);
  CHECK_EQ(s.n, 6u);
  CHECK_EQ(s.wins, 3u);
  CHECK_NEAR(s.total, 130.0, 1e-9);
  CHECK_NEAR(s.profit_factor, 330.0 / 200.0, 1e-9);
  CHECK_NEAR(s.max_drawdown, 150.0, 1e-9);          // 250 peak -> 100 after the two losses... then -100
  CHECK_NEAR(s.total_ex_top3, 130.0 - 330.0, 1e-9);  // relies on the three winners
  CHECK_NEAR(s.expectancy, 130.0 / 6, 1e-9);
  CHECK_EQ(s.span_days, 11);
}

TEST(report_loads_journal_and_renders_verdict) {
  const std::string dir = "build/test_tmp/report_journal";
  std::filesystem::remove_all(dir);
  write(dir + "/2026-09-24/news.jsonl",
        "{\"src\":\"globenewswire\",\"kind\":\"news\",\"catalyst\":\"contract\",\"score\":80,\"sym\":\"ACMR\","
        "\"in_universe\":true,\"pub_to_recv_ms\":4000}\n"
        "{\"src\":\"edgar_8k\",\"kind\":\"filing_doc\",\"catalyst\":\"contract\",\"score\":80,\"sym\":\"ACMR\","
        "\"in_universe\":true,\"dup\":true,\"first_src\":\"globenewswire\",\"behind_first_ms\":1500}\n"
        "not json\n");
  write(dir + "/2026-09-24/signals.jsonl", "{\"tier\":\"HIGH\",\"catalyst\":\"contract\",\"score\":80}\n");
  write(dir + "/2026-09-24/outcomes.jsonl",
        "{\"tier_reached\":\"HIGH\",\"catalyst\":\"contract\",\"score\":80,\"horizon_s\":300,\"move_pct\":12,"
        "\"max_move_pct\":20,\"min_move_pct\":-2}\n");
  write(dir + "/2026-09-24/trades.jsonl", trade("HIGH", "target", 150, 10) + trade("HIGH", "stop", -60, 20) +
                                              "{\"event\":\"open\",\"ticker\":\"X\"}\n");
  write(dir + "/2026-09-25/trades.jsonl", trade("ALERT", "time", 20, 30));
  const JournalData d = load_journal(dir);
  CHECK_EQ(d.days.size(), 2u);
  CHECK_EQ(d.news.size(), 2u);
  CHECK_EQ(d.bad_lines, 1u);
  CHECK_EQ(d.trades.size(), 3u);  // open records are ignored
  const std::string rep = render_report(d);
  CHECK(rep.find("globenewswire") != std::string::npos);
  CHECK(rep.find("tier HIGH @5m") != std::string::npos);
  CHECK(rep.find("trades 3") != std::string::npos);
  CHECK(rep.find("[FAIL] 3 paper trades") != std::string::npos);
  CHECK(rep.find("verdict: not yet") != std::string::npos);
  const JournalData only25 = load_journal(dir, "2026-09-25");
  CHECK_EQ(only25.trades.size(), 1u);
  GoNoGo lax;
  lax.min_trades = 3;
  lax.min_span_days = 1;
  lax.min_profit_factor = 1.3;
  // Without the top 3 winners (all winners here) the total is negative -> still not met.
  CHECK(render_report(d, lax).find("verdict: not yet") != std::string::npos);
}
