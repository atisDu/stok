#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/events.hpp"
#include "ref/symbols.hpp"

namespace stok::research {

// Rebuilds a past day's EDGAR news stream from the SEC's official archives,
// so days you never recorded (e.g. Nasdaq's sample ITCH dates) can be
// backtested:
//   daily-index master.YYYYMMDD.idx  -> the day's 8-K / 6-K (+ dilution) filings
//   each filing's -index.htm         -> acceptance time, 8-K items, EX-99 link
//   the EX-99 exhibit                -> press-release text
// Events are stamped at acceptance time + a dissemination delay.
struct EdgarHistoryOptions {
  bool listed_only = true;               // issuers that map to an exchange-listed security
  bool fetch_exhibits = true;
  bool dilution_forms = true;            // also 424B*, S-1, S-3, EFFECT (intraday dilution flags)
  int64_t dissemination_delay_ms = 30000;  // acceptance -> visible on the feed (conservative)
  int64_t exhibit_delay_ms = 2000;         // our exhibit fetch after the filing appeared
  std::size_t max_filings = 0;             // 0 = no cap (for quick tests)
};

// Returns the body for a URL, or nullopt (404 / error).
using FetchFn = std::function<std::optional<std::string>(const std::string& url)>;

struct EdgarDayStats {
  std::size_t index_rows = 0, candidates = 0, filings = 0, exhibits = 0, missing_accepted = 0, errors = 0;
};

// Appends the day's events to `out` (sorted by arrival time).
EdgarDayStats build_edgar_day(const std::string& ymd, const SymbolTable& symbols, const EdgarHistoryOptions& o,
                              const FetchFn& fetch, std::vector<NewsEvent>& out);

// "…<div class="infoHead">Accepted</div><div class="info">2019-01-30 16:05:12</div>…" -> epoch ns (ET)
std::optional<int64_t> parse_index_accepted(std::string_view index_html);
// Items listed on an 8-K index page.
uint32_t parse_index_items(std::string_view index_html);

std::string master_index_url(const std::string& ymd);
// "edgar/data/1234567/0001234567-26-000012.txt" -> ".../data/1234567/000123456726000012/0001234567-26-000012-index.htm"
std::string filing_index_url(std::string_view master_filename);

}  // namespace stok::research
