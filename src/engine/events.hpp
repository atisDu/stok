#pragma once

#include <cstdint>
#include <type_traits>

#include "core/fixed_string.hpp"

namespace stok {

enum class EventKind : uint8_t {
  News = 0,       // press release / wire story
  Filing = 1,     // EDGAR filing notice (form + 8-K items)
  FilingDoc = 2,  // text of an EDGAR exhibit (e.g. EX-99.1 press release)
  Halt = 3,       // trading halt / resumption (Nasdaq Trader RSS)
};

inline const char* event_kind_name(EventKind k) {
  switch (k) {
    case EventKind::News: return "news";
    case EventKind::Filing: return "filing";
    case EventKind::FilingDoc: return "filing_doc";
    case EventKind::Halt: return "halt";
  }
  return "?";
}

struct HaltInfo {
  char reason[8];        // T1, T2, LUDP, H10, M, ...
  char market[12];       // NASDAQ, NYSE, ...
  int64_t halt_ns;       // halt time (epoch ns)
  int64_t resume_quote_ns;
  int64_t resume_trade_ns;  // 0 = not yet scheduled
  double pause_threshold;
};

inline constexpr int kMaxTickers = 6;
inline constexpr int kMaxUnresolved = 3;

// One item from any source. Fixed size and trivially copyable, so it moves
// through the lock-free rings by memcpy with no heap allocation.
struct NewsEvent {
  uint64_t id_hash;     // per-source dedupe id (guid / accession / link)
  uint64_t title_key;   // headline_key(title), for cross-source dedupe
  uint64_t recv_ns;     // wall clock: response containing this item completed
  uint64_t sent_ns;     // wall clock: the poll request that returned it was sent
  int64_t published_ns; // source's own timestamp (0 if absent)
  uint64_t parsed_ns;   // wall clock: event handed to the engine
  uint32_t cik;
  uint32_t items_mask;  // 8-K item bits (see edgar.hpp)
  uint16_t source;      // feed index
  EventKind kind;
  uint8_t n_tickers;
  uint8_t n_unresolved;
  uint8_t pad_[3];
  uint32_t tickers[kMaxTickers];         // symbol ids
  char unresolved[kMaxUnresolved][16];   // e.g. "OTC:ABCDF", "TSXV:XYZ"
  char form[16];                         // EDGAR form type
  HaltInfo halt;
  FixedStr<256> title;
  FixedStr<320> link;
  FixedStr<3072> body;

  void reset() noexcept {
    id_hash = title_key = recv_ns = sent_ns = parsed_ns = 0;
    published_ns = 0;
    cik = items_mask = 0;
    source = 0;
    kind = EventKind::News;
    n_tickers = n_unresolved = 0;
    form[0] = '\0';
    halt = HaltInfo{};
    title.clear();
    link.clear();
    body.clear();
  }
};

static_assert(std::is_trivially_copyable_v<NewsEvent>);

// Market-side events the engine must react to (not per-trade updates, which
// go through the seqlocked MarketBoard).
struct MarketEvent {
  enum class Type : uint8_t { TradingAction = 0, SystemEvent = 1 };
  Type type;
  char state;        // ITCH trading state: H halted, P paused, Q quote-only, T trading
  char reason[5];    // e.g. "T1", "LUDP"
  char sys_code;     // ITCH system event code
  uint32_t sym;
  int64_t ts_ns;     // epoch ns
};

}  // namespace stok
