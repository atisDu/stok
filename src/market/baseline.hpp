#pragma once

#include <string>

#include "market/board.hpp"
#include "ref/symbols.hpp"

namespace stok {

// Self-built daily baseline: at the end of each session the daemon appends
// every traded symbol's volume and close to history.tsv and recomputes
// baseline.tsv (prev close, 20-day average volume). RVOL then compares
// like with like: the same venue(s) the live feed covers. No external
// history vendor needed.
class BaselineStore {
 public:
  explicit BaselineStore(std::string dir) : dir_(std::move(dir)) {}

  // Appends one session from the board (writer view: call on the market
  // thread or after it stopped). Returns rows written.
  std::size_t append_session(const std::string& date_ymd, const MarketBoard& board, const SymbolTable& symbols);
  // Rebuilds baseline.tsv from history.tsv (last `days` sessions).
  bool rebuild(int days = 20, std::string* report = nullptr);

  std::string baseline_path() const;
  std::string history_path() const;

 private:
  std::string dir_;
};

}  // namespace stok
