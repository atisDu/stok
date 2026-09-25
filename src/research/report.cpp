#include "research/report.hpp"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "core/ascii.hpp"
#include "util/file.hpp"
#include "util/json.hpp"
#include "util/time.hpp"

namespace stok::research {

namespace {

template <typename F>
void each_line(const std::string& path, JournalData& d, F&& f) {
  auto text = fileutil::read_file(path);
  if (!text) return;
  JsonDoc doc;
  for_each_line(*text, [&](std::string_view line) {
    if (trim(line).empty()) return;
    if (!doc.parse(line) || doc.root().type != JsonDoc::Type::Object) {
      ++d.bad_lines;
      return;
    }
    f(doc);
  });
}

std::string s(const JsonDoc& d, const char* k) { return JsonDoc::str(d.get(d.root(), k)); }
double n(const JsonDoc& d, const char* k, double def = 0) { return JsonDoc::num(d.get(d.root(), k), def); }
bool b(const JsonDoc& d, const char* k) {
  const auto* v = d.get(d.root(), k);
  return v && v->type == JsonDoc::Type::Bool && v->boolean;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

double mean(const std::vector<double>& v) {
  if (v.empty()) return 0;
  double t = 0;
  for (double x : v) t += x;
  return t / static_cast<double>(v.size());
}

void line(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void line(std::string& out, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  out += buf;
  out += '\n';
}

std::string score_bucket(int score) {
  if (score >= 85) return "85+";
  if (score >= 75) return "75-84";
  if (score >= 65) return "65-74";
  if (score >= 55) return "55-64";
  return "<55";
}

}  // namespace

JournalData load_journal(const std::string& dir, const std::string& from, const std::string& to) {
  JournalData d;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_directory()) continue;
    const std::string day = e.path().filename().string();
    int y;
    unsigned m, dd;
    if (!timeutil::parse_ymd(day, y, m, dd)) continue;
    if (!from.empty() && day < from) continue;
    if (!to.empty() && day > to) continue;
    d.days.push_back(day);
  }
  std::sort(d.days.begin(), d.days.end());
  for (const auto& day : d.days) {
    const std::string base = fileutil::join(dir, day);
    each_line(fileutil::join(base, "news.jsonl"), d, [&](const JsonDoc& j) {
      NewsRow r;
      r.day = day;
      r.src = s(j, "src");
      r.kind = s(j, "kind");
      r.catalyst = s(j, "catalyst");
      r.score = static_cast<int>(n(j, "score"));
      r.has_sym = !s(j, "sym").empty();
      r.in_universe = b(j, "in_universe");
      r.dup = b(j, "dup");
      r.first_src = s(j, "first_src");
      r.behind_ms = n(j, "behind_first_ms");
      r.pub_to_recv_ms = n(j, "pub_to_recv_ms", -1);
      d.news.push_back(std::move(r));
    });
    each_line(fileutil::join(base, "signals.jsonl"), d, [&](const JsonDoc& j) {
      SignalRow r;
      r.day = day;
      r.tier = s(j, "tier");
      r.catalyst = s(j, "catalyst");
      r.score = static_cast<int>(n(j, "score"));
      r.news_to_signal_ms = n(j, "news_to_signal_ms", -1);
      d.signals.push_back(std::move(r));
    });
    each_line(fileutil::join(base, "outcomes.jsonl"), d, [&](const JsonDoc& j) {
      OutcomeRow r;
      r.day = day;
      r.tier = s(j, "tier_reached");
      r.catalyst = s(j, "catalyst");
      r.score = static_cast<int>(n(j, "score"));
      r.horizon_s = static_cast<int>(n(j, "horizon_s"));
      r.move = n(j, "move_pct");
      r.mfe = n(j, "max_move_pct");
      r.mae = n(j, "min_move_pct");
      d.outcomes.push_back(std::move(r));
    });
    each_line(fileutil::join(base, "trades.jsonl"), d, [&](const JsonDoc& j) {
      if (s(j, "event") != "close") return;
      TradeRow r;
      r.day = day;
      r.ticker = s(j, "ticker");
      r.tier = s(j, "tier");
      r.catalyst = s(j, "catalyst");
      r.exit_reason = s(j, "exit_reason");
      r.score = static_cast<int>(n(j, "score"));
      r.exit_ns = static_cast<uint64_t>(n(j, "exit_ns"));
      r.pnl_usd = n(j, "pnl_usd");
      r.pnl_pct = n(j, "pnl_pct");
      r.mfe = n(j, "mfe_pct");
      r.mae = n(j, "mae_pct");
      r.hold_s = n(j, "hold_s");
      d.trades.push_back(std::move(r));
    });
  }
  return d;
}

TradeStats trade_stats(std::vector<TradeRow> trades) {
  TradeStats st;
  st.n = trades.size();
  if (trades.empty()) return st;
  std::sort(trades.begin(), trades.end(), [](const TradeRow& a, const TradeRow& b) { return a.exit_ns < b.exit_ns; });
  double equity = 0, peak = 0, pct_sum = 0;
  std::vector<double> pnls;
  for (const auto& t : trades) {
    pnls.push_back(t.pnl_usd);
    pct_sum += t.pnl_pct;
    if (t.pnl_usd > 0) {
      ++st.wins;
      st.gross_win += t.pnl_usd;
    } else {
      ++st.losses;
      st.gross_loss += -t.pnl_usd;
    }
    equity += t.pnl_usd;
    peak = std::max(peak, equity);
    st.max_drawdown = std::max(st.max_drawdown, peak - equity);
  }
  st.total = equity;
  st.win_rate = static_cast<double>(st.wins) / static_cast<double>(st.n);
  st.avg_win = st.wins ? st.gross_win / static_cast<double>(st.wins) : 0;
  st.avg_loss = st.losses ? st.gross_loss / static_cast<double>(st.losses) : 0;
  st.expectancy = st.total / static_cast<double>(st.n);
  st.expectancy_pct = pct_sum / static_cast<double>(st.n);
  st.profit_factor = st.gross_loss > 0 ? st.gross_win / st.gross_loss : (st.gross_win > 0 ? INFINITY : 0);
  std::sort(pnls.begin(), pnls.end(), std::greater<double>());
  st.total_ex_top3 = st.total;
  for (std::size_t i = 0; i < 3 && i < pnls.size(); ++i)
    if (pnls[i] > 0) st.total_ex_top3 -= pnls[i];
  int y0, y1;
  unsigned m0, d0, m1, d1;
  if (timeutil::parse_ymd(trades.front().day, y0, m0, d0) && timeutil::parse_ymd(trades.back().day, y1, m1, d1))
    st.span_days = static_cast<int>(timeutil::days_from_civil(y1, m1, d1) - timeutil::days_from_civil(y0, m0, d0)) + 1;
  return st;
}

std::string render_report(const JournalData& d, const GoNoGo& g) {
  std::string out;
  line(out, "stok report: %zu day(s)%s%s%s", d.days.size(), d.days.empty() ? "" : " (",
       d.days.empty() ? "" : (d.days.front() + " .. " + d.days.back()).c_str(), d.days.empty() ? "" : ")");
  if (d.bad_lines) line(out, "warning: %zu unparseable journal lines skipped", d.bad_lines);

  // ---- sources ----
  line(out, "\n== Sources (who publishes first) ==");
  line(out, "%-18s %8s %8s %8s %12s %14s", "source", "items", "first", "behind", "behind p50", "pub->recv p50");
  std::map<std::string, std::vector<const NewsRow*>> by_src;
  for (const auto& r : d.news) by_src[r.src].push_back(&r);
  for (const auto& [src, rows] : by_src) {
    std::size_t first = 0, behind = 0;
    std::vector<double> lag, pub;
    for (const auto* r : rows) {
      if (r->kind == "halt") continue;
      if (r->dup && r->first_src != r->src) {
        ++behind;
        lag.push_back(r->behind_ms);
      } else if (!r->dup) {
        ++first;
      }
      if (r->pub_to_recv_ms >= 0) pub.push_back(r->pub_to_recv_ms / 1000.0);
    }
    line(out, "%-18s %8zu %8zu %8zu %10.0fms %13.1fs", src.c_str(), rows.size(), first, behind, median(lag), median(pub));
  }

  // ---- funnel ----
  std::size_t with_sym = 0, in_univ = 0, watchable = 0;
  std::map<std::string, std::size_t> cats;
  for (const auto& r : d.news) {
    if (r.kind == "halt") continue;
    with_sym += r.has_sym;
    in_univ += r.in_universe;
    if (r.in_universe && r.score >= 55) {
      ++watchable;
      ++cats[r.catalyst];
    }
  }
  line(out, "\n== Funnel ==");
  line(out, "stories %zu -> with ticker %zu -> in universe %zu -> score >= 55: %zu", d.news.size(), with_sym, in_univ,
       watchable);
  std::string cat_line = "  by catalyst:";
  for (const auto& [c, k] : cats) cat_line += " " + c + "=" + std::to_string(k);
  if (!cats.empty()) line(out, "%s", cat_line.c_str());
  std::map<std::string, std::size_t> tiers;
  for (const auto& sgn : d.signals) ++tiers[sgn.tier];
  std::string tier_line = "signals:";
  for (const auto& [t, k] : tiers) tier_line += " " + t + "=" + std::to_string(k);
  line(out, "%s", tier_line.c_str());

  // ---- outcomes ----
  line(out, "\n== Price after the news (from the last print before it) ==");
  line(out, "%-26s %6s %8s %8s %7s %8s %8s", "group @ horizon", "n", "mean%", "median%", "up%", "MFE%", "MAE%");
  auto emit_group = [&](const std::string& name, const std::vector<const OutcomeRow*>& rows) {
    if (rows.empty()) return;
    std::vector<double> mv, mfe, mae;
    std::size_t up = 0;
    for (const auto* r : rows) {
      mv.push_back(r->move);
      mfe.push_back(r->mfe);
      mae.push_back(r->mae);
      up += r->move > 0;
    }
    line(out, "%-26s %6zu %+8.2f %+8.2f %6.0f%% %+8.2f %+8.2f", name.c_str(), rows.size(), mean(mv), median(mv),
         100.0 * static_cast<double>(up) / static_cast<double>(rows.size()), mean(mfe), mean(mae));
  };
  std::map<int, std::map<std::string, std::vector<const OutcomeRow*>>> by_h_tier, by_h_cat, by_h_score;
  for (const auto& r : d.outcomes) {
    by_h_tier[r.horizon_s][r.tier].push_back(&r);
    by_h_cat[r.horizon_s][r.catalyst].push_back(&r);
    by_h_score[r.horizon_s][score_bucket(r.score)].push_back(&r);
  }
  for (const auto& [h, groups] : by_h_tier)
    for (const auto& [name, rows] : groups) emit_group("tier " + name + " @" + std::to_string(h / 60) + "m", rows);
  for (int h : {300, 1800}) {
    if (!by_h_cat.count(h)) continue;
    for (const auto& [name, rows] : by_h_cat[h]) emit_group(name + " @" + std::to_string(h / 60) + "m", rows);
    for (const auto& [name, rows] : by_h_score[h]) emit_group("score " + name + " @" + std::to_string(h / 60) + "m", rows);
  }
  if (d.outcomes.empty()) line(out, "(no outcomes yet: they start 1 minute after a watched story and need market data)");

  // ---- paper trading ----
  line(out, "\n== Paper trading ==");
  const TradeStats st = trade_stats(d.trades);
  if (st.n == 0) {
    line(out, "(no closed paper trades: enable [paper] with market data)");
  } else {
    line(out, "trades %zu | win rate %.0f%% | avg win $%.2f | avg loss $%.2f | expectancy $%.2f (%+.2f%%) per trade",
         st.n, st.win_rate * 100, st.avg_win, st.avg_loss, st.expectancy, st.expectancy_pct);
    line(out, "total $%.2f | profit factor %.2f | max drawdown $%.2f | without top 3 winners $%.2f | span %d days",
         st.total, st.profit_factor, st.max_drawdown, st.total_ex_top3, st.span_days);
    std::map<std::string, std::vector<TradeRow>> by_reason, by_tier;
    for (const auto& t : d.trades) {
      by_reason[t.exit_reason].push_back(t);
      by_tier[t.tier].push_back(t);
    }
    for (const auto* grp : {&by_tier, &by_reason}) {
      for (const auto& [k, rows] : *grp) {
        const TradeStats gs = trade_stats(rows);
        line(out, "  %-10s n=%-5zu win %3.0f%%  expectancy $%8.2f  PF %5.2f  total $%9.2f", k.c_str(), gs.n,
             gs.win_rate * 100, gs.expectancy, gs.profit_factor, gs.total);
      }
    }
  }

  // ---- go / no-go ----
  line(out, "\n== Go / no-go for real money (PLAN.md section 5) ==");
  auto chk = [&](bool ok, const std::string& what) { line(out, "  [%s] %s", ok ? "PASS" : "FAIL", what.c_str()); };
  chk(st.n >= static_cast<std::size_t>(g.min_trades),
      std::to_string(st.n) + " paper trades (need >= " + std::to_string(g.min_trades) + ")");
  chk(st.span_days >= g.min_span_days,
      std::to_string(st.span_days) + " days of trading (need >= " + std::to_string(g.min_span_days) + ")");
  chk(st.n > 0 && st.expectancy > 0, "positive expectancy after simulated slippage and fees");
  char pf[64];
  std::snprintf(pf, sizeof(pf), "profit factor %.2f (need >= %.2f)", st.profit_factor, g.min_profit_factor);
  chk(st.n > 0 && st.profit_factor >= g.min_profit_factor, pf);
  chk(st.n > 0 && st.total_ex_top3 > 0, "still profitable without the 3 best trades");
  const bool all = st.n >= static_cast<std::size_t>(g.min_trades) && st.span_days >= g.min_span_days &&
                   st.expectancy > 0 && st.profit_factor >= g.min_profit_factor && st.total_ex_top3 > 0;
  line(out, "verdict: %s", all ? "criteria met: consider small, semi-automatic live trading with hard limits"
                               : "not yet: keep paper trading / tuning; do not risk real money");
  return out;
}

}  // namespace stok::research
