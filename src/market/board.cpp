#include "market/board.hpp"

#include <cstring>

#include "util/time.hpp"

namespace stok {

uint64_t MarketState::volume_since_minute(int from_minute) const {
  uint64_t v = 0;
  for (const MinuteBar& b : bars)
    if (b.minute >= from_minute) v += b.volume;
  return v;
}

int32_t MarketState::price_at_minute(int minute) const {
  int best_min = -1;
  int32_t px = 0;
  for (const MinuteBar& b : bars) {
    if (b.minute >= 0 && b.minute <= minute && b.minute > best_min) {
      best_min = b.minute;
      px = b.close_px;
    }
  }
  return px;
}

MarketBoard::MarketBoard(std::size_t n_symbols) : states_(n_symbols) {
  for (auto& s : states_) {
    MarketState& st = s.begin_write();
    for (auto& b : st.bars) b.minute = -1;
    s.end_write();
  }
}

void MarketBoard::set_session_date(int y, unsigned m, unsigned d) {
  midnight_ns_ = timeutil::eastern_midnight_ns(y, m, d);
}

STOK_ALWAYS_INLINE void MarketBoard::apply_trade(MarketState& s, int32_t px, uint64_t shares, uint64_t ns) {
  MarketHot& h = s.hot;
  h.last_px = px;
  if (h.high_px == 0 || px > h.high_px) h.high_px = px;
  if (h.low_px == 0 || px < h.low_px) h.low_px = px;
  h.day_volume += shares;
  h.notional_e4 += static_cast<uint64_t>(px) * shares;
  ++h.trades;
  h.last_trade_ns = midnight_ns_ + static_cast<int64_t>(ns);
  if (ns < kRthOpenNs) {
    if (px > h.pm_high_px) h.pm_high_px = px;
  } else if (ns < kRthCloseNs) {
    if (h.open_px == 0) h.open_px = px;
    h.rth_last_px = px;
  }
  const int minute = static_cast<int>(ns / kNsPerMinute);
  MinuteBar& b = s.bars[minute & (kBarRing - 1)];
  if (b.minute != minute) {
    b.minute = minute;
    b.open_px = b.high_px = b.low_px = b.close_px = px;
    b.volume = 0;
  }
  if (px > b.high_px) b.high_px = px;
  if (px < b.low_px) b.low_px = px;
  b.close_px = px;
  b.volume += shares;
}

void MarketBoard::on_trade(uint32_t sym, int32_t px, uint64_t shares, uint64_t ns) {
  if (sym >= states_.size() || px <= 0 || shares == 0) return;
  MarketState& s = states_[sym].begin_write();
  apply_trade(s, px, shares, ns);
  states_[sym].end_write();
}

void MarketBoard::on_cross(uint32_t sym, int32_t px, uint64_t shares, uint64_t ns, char cross_type) {
  if (sym >= states_.size() || px <= 0) return;
  MarketState& s = states_[sym].begin_write();
  if (shares > 0) apply_trade(s, px, shares, ns);
  if (cross_type == 'O') s.hot.open_px = px;
  else if (cross_type == 'C') s.hot.close_px = px;
  states_[sym].end_write();
}

void MarketBoard::on_trading_action(uint32_t sym, char state, const char* reason4) {
  if (sym >= states_.size()) return;
  MarketState& s = states_[sym].begin_write();
  s.hot.trading_state = state;
  int n = 0;
  for (; n < 4 && reason4[n] && reason4[n] != ' '; ++n) s.hot.halt_reason[n] = reason4[n];
  s.hot.halt_reason[n] = '\0';
  states_[sym].end_write();
}

void MarketBoard::on_reg_sho(uint32_t sym, char action) {
  if (sym >= states_.size()) return;
  MarketState& s = states_[sym].begin_write();
  s.hot.reg_sho_restricted = action == '1' || action == '2';
  states_[sym].end_write();
}

}  // namespace stok
