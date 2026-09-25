#include "market/itch_book.hpp"

#include <cstring>

namespace stok {

ItchBook::ItchBook(const SymbolTable& symbols, MarketBoard& board, SpscQueue<MarketEvent>* events, Waker* waker,
                   std::size_t order_capacity, bool track_quotes)
    : symbols_(symbols),
      board_(board),
      events_(events),
      waker_(waker),
      orders_(order_capacity, 0.6),
      books_(track_quotes ? board.size() : 0),
      track_quotes_(track_quotes) {
  locate_to_sym_.fill(SymbolTable::kInvalid);
  last_state_.fill(0);
}

void ItchBook::on_system_event(uint64_t ts, char code) {
  ++st_.messages;
  last_sys_ = code;
  if (!events_) return;
  if (MarketEvent* e = events_->try_claim()) {
    *e = MarketEvent{};
    e->type = MarketEvent::Type::SystemEvent;
    e->sys_code = code;
    e->sym = SymbolTable::kInvalid;
    e->ts_ns = board_.midnight_ns() + static_cast<int64_t>(ts);
    events_->publish();
    if (waker_) waker_->notify();
  }
}

void ItchBook::on_stock_directory(uint16_t locate, uint64_t, uint64_t key, char, char, uint32_t) {
  ++st_.messages;
  locate_to_sym_[locate] = symbols_.find_key(key);
}

void ItchBook::on_trading_action(uint16_t locate, uint64_t ts, uint64_t key, char state, const char* reason) {
  ++st_.messages;
  uint32_t sym = locate_to_sym_[locate];
  if (sym == SymbolTable::kInvalid) {
    sym = symbols_.find_key(key);
    locate_to_sym_[locate] = sym;
  }
  if (sym == SymbolTable::kInvalid) return;
  board_.on_trading_action(sym, state, reason);
  const char prev = last_state_[locate];
  last_state_[locate] = state;
  // The start-of-day burst sets every symbol to 'T': only forward real halts,
  // pauses and resumptions from them.
  const bool interesting = state != 'T' || (prev != 0 && prev != 'T');
  if (!interesting) return;
  ++st_.halts;
  if (!events_) return;
  if (MarketEvent* e = events_->try_claim()) {
    *e = MarketEvent{};
    e->type = MarketEvent::Type::TradingAction;
    e->state = state;
    int n = 0;
    for (; n < 4 && reason[n] != ' ' && reason[n] != '\0'; ++n) e->reason[n] = reason[n];
    e->reason[n] = '\0';
    e->sym = sym;
    e->ts_ns = board_.midnight_ns() + static_cast<int64_t>(ts);
    events_->publish();
    if (waker_) waker_->notify();
  }
}

void ItchBook::on_reg_sho(uint16_t locate, uint64_t, char action) {
  ++st_.messages;
  const uint32_t sym = locate_to_sym_[locate];
  if (sym != SymbolTable::kInvalid) board_.on_reg_sho(sym, action);
}

}  // namespace stok
