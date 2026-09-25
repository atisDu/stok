#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/events.hpp"
#include "ref/symbols.hpp"

namespace stok::research {

struct LoadedNews {
  std::vector<NewsEvent> events;  // sorted by recv_ns
  std::size_t lines = 0;
  std::size_t bad_lines = 0;
  std::size_t dropped_tickers = 0;  // tickers not in the symbol table used for the replay
};

// Rebuilds NewsEvents from a journal news.jsonl so a recorded session can be
// replayed. Tickers are re-resolved by symbol against `symbols` (ids differ
// between runs). `sources` maps source names to indices; unknown names are
// appended.
LoadedNews load_news_events(std::string_view jsonl, const SymbolTable& symbols, std::vector<std::string>& sources);
LoadedNews load_news_file(const std::string& path, const SymbolTable& symbols, std::vector<std::string>& sources);

}  // namespace stok::research
