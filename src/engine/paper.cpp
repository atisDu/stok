#include "engine/paper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "util/time.hpp"

namespace stok {

PaperTrader::PaperTrader(PaperConfig cfg, const SymbolTable& symbols, const MarketBoard& board)
    : cfg_(cfg), symbols_(symbols), board_(board), pos_(static_cast<std::size_t>(std::max(1, cfg.max_open))) {}

double PaperTrader::fees(double shares) const {
  return std::max(cfg_.commission_min, shares * cfg_.commission_per_share);
}

std::size_t PaperTrader::open_positions() const {
  std::size_t n = 0;
  for (const auto& p : pos_) n += p.active ? 1 : 0;
  return n;
}

void PaperTrader::roll_day(uint64_t now) {
  if (day_ != 0 && now >= day_checked_at_ && now - day_checked_at_ < 60 * kNsPerSec) return;
  day_checked_at_ = now;
  int y;
  unsigned m, d;
  timeutil::eastern_date(static_cast<int64_t>(now), y, m, d);
  const int32_t day = static_cast<int32_t>(timeutil::days_from_civil(y, m, d));
  if (day != day_) {
    day_ = day;
    st_.day_realized = 0;
  }
}

void PaperTrader::on_signal(const Signal& s, uint64_t now) {
  if (!((s.tier == Tier::Alert && cfg_.trade_alert) || (s.tier == Tier::High && cfg_.trade_high))) return;
  roll_day(now);
  ++st_.signals;
  Pos* slot = nullptr;
  for (auto& p : pos_) {
    if (p.active && p.t.sym == s.sym) {
      ++st_.rejected_dup;  // one position per ticker (an ALERT->HIGH upgrade doesn't add)
      return;
    }
    if (!p.active && !slot) slot = &p;
  }
  if (st_.day_realized <= -cfg_.max_daily_loss_usd) {
    ++st_.rejected_risk;
    return;
  }
  if (!slot) {
    ++st_.rejected_capacity;
    return;
  }
  *slot = Pos{};
  slot->active = true;
  slot->eligible_ns = now + static_cast<uint64_t>(cfg_.latency_ms) * kNsPerMs;
  PaperTrade& t = slot->t;
  t.sym = s.sym;
  t.tier = s.tier;
  t.catalyst = s.catalyst;
  t.score = s.score;
  t.news_id = s.news_id;
  t.signal_ns = now;
  t.signal_px = s.ask > 0 ? s.ask : s.price;
  std::snprintf(t.ticker, sizeof(t.ticker), "%s", s.ticker);
}

bool PaperTrader::try_fill(Pos& p, uint64_t now) {
  PaperTrade& t = p.t;
  const MarketHot h = board_.hot(t.sym);
  const bool timed_out = now - p.eligible_ns > static_cast<uint64_t>(cfg_.entry_timeout_s) * kNsPerSec;
  const bool quote = h.quote_valid();
  const double px = quote ? h.ask() : h.last();
  if (h.halted() || px <= 0) {
    if (timed_out) {
      ++st_.cancelled_timeout;
      p.active = false;
    }
    return false;
  }
  if (t.signal_px > 0 && px > t.signal_px * (1.0 + cfg_.max_chase_pct / 100.0)) {
    ++st_.cancelled_chase;  // it ran away during our latency: don't chase
    p.active = false;
    return false;
  }
  const double fill = px * (1.0 + cfg_.slippage_bps / 1e4);
  const double shares = std::floor(cfg_.position_usd / fill);
  if (shares < 1) {
    p.active = false;
    return false;
  }
  t.entry_px = fill;
  t.entry_ns = now;
  t.shares = shares;
  t.quote_entry = quote;
  t.size_over_touch = quote && shares > h.ask_sz;
  t.fees = fees(shares);
  t.mfe_pct = t.mae_pct = 0;
  t.closed = false;
  p.filled = true;
  ++st_.opened;
  LOG_INFO("paper: BUY %s %.0f @ %.4f (%s, score %d, %s)", t.ticker, shares, fill, tier_name(t.tier), t.score,
           quote ? "at ask" : "at last");
  return true;
}

bool PaperTrader::check_exit(Pos& p, uint64_t now, const char*& reason, double& px, bool& quote) {
  PaperTrade& t = p.t;
  const MarketHot h = board_.hot(t.sym);
  const bool flat_time = timeutil::eastern_minute_of_day(static_cast<int64_t>(now)) >= cfg_.flat_by_minute &&
                         timeutil::eastern_minute_of_day(static_cast<int64_t>(t.entry_ns)) < cfg_.flat_by_minute;
  if (h.halted()) return false;  // can't trade a halted stock
  quote = h.quote_valid();
  const double mark = quote ? h.bid() : h.last();
  if (mark <= 0) return false;
  const double chg = (mark - t.entry_px) * 100.0 / t.entry_px;
  t.mfe_pct = std::max(t.mfe_pct, chg);
  t.mae_pct = std::min(t.mae_pct, chg);
  px = mark * (1.0 - cfg_.slippage_bps / 1e4);
  if (chg <= -cfg_.stop_loss_pct) reason = "stop";
  else if (chg >= cfg_.take_profit_pct) reason = "target";
  else if (t.mfe_pct >= cfg_.trail_activate_pct && chg <= t.mfe_pct - cfg_.trail_pct) reason = "trail";
  else if (now - t.entry_ns >= static_cast<uint64_t>(cfg_.max_hold_min) * 60 * kNsPerSec) reason = "time";
  else if (flat_time) reason = "flat";
  return reason != nullptr;
}

void PaperTrader::finish(Pos& p, uint64_t now, const char* reason, double px, bool quote) {
  PaperTrade& t = p.t;
  t.exit_px = px;
  t.exit_ns = now;
  t.quote_exit = quote;
  t.closed = true;
  t.fees += fees(t.shares);
  t.pnl_usd = (t.exit_px - t.entry_px) * t.shares - t.fees;
  t.pnl_pct = t.pnl_usd * 100.0 / (t.entry_px * t.shares);
  std::snprintf(t.exit_reason, sizeof(t.exit_reason), "%s", reason);
  ++st_.closed;
  if (t.pnl_usd > 0) {
    ++st_.wins;
    st_.gross_win += t.pnl_usd;
  } else {
    ++st_.losses;
    st_.gross_loss += -t.pnl_usd;
  }
  st_.realized += t.pnl_usd;
  st_.day_realized += t.pnl_usd;
  st_.peak = std::max(st_.peak, st_.realized);
  st_.max_drawdown = std::max(st_.max_drawdown, st_.peak - st_.realized);
  LOG_INFO("paper: SELL %s %.0f @ %.4f (%s) pnl $%.2f (%+.2f%%)", t.ticker, t.shares, t.exit_px, reason, t.pnl_usd,
           t.pnl_pct);
}

std::string PaperTrader::stats_line() const {
  char b[320];
  const double pf = st_.gross_loss > 0 ? st_.gross_win / st_.gross_loss : (st_.gross_win > 0 ? 99.0 : 0.0);
  std::snprintf(b, sizeof(b),
                "paper: signals=%llu opened=%llu closed=%llu open=%zu win=%llu loss=%llu pnl=$%.2f day=$%.2f "
                "PF=%.2f maxDD=$%.2f | skipped risk=%llu cap=%llu dup=%llu chase=%llu timeout=%llu",
                (unsigned long long)st_.signals, (unsigned long long)st_.opened, (unsigned long long)st_.closed,
                open_positions(), (unsigned long long)st_.wins, (unsigned long long)st_.losses, st_.realized,
                st_.day_realized, pf, st_.max_drawdown, (unsigned long long)st_.rejected_risk,
                (unsigned long long)st_.rejected_capacity, (unsigned long long)st_.rejected_dup,
                (unsigned long long)st_.cancelled_chase, (unsigned long long)st_.cancelled_timeout);
  return b;
}

}  // namespace stok
