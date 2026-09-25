#pragma once

#include <atomic>
#include <cstring>
#include <string>

#include "market/itch.hpp"
#include "market/sources.hpp"
#include "ref/symbols.hpp"

namespace stok {

// Reads the stock directory ('R' messages, sent before trading starts) from
// an ITCH file and adds any symbol missing from `symbols`. Historical
// sessions then resolve tickers that have since been delisted or renamed,
// which avoids survivorship bias in replays.
struct ItchDirectoryCollector {
  SymbolTable* symbols;
  uint64_t seen = 0;
  std::size_t added = 0;
  bool done = false;
  void on_system_event(uint64_t, char) {}
  void on_stock_directory(uint16_t, uint64_t, uint64_t key, char cat, char fin, uint32_t round_lot) {
    ++seen;
    if (symbols->find_key(key) != SymbolTable::kInvalid) return;
    char sym[9];
    std::memcpy(sym, &key, 8);
    int n = 8;
    while (n > 0 && sym[n - 1] == ' ') --n;
    sym[n] = '\0';
    SymbolInfo s;
    s.ticker = sym;
    s.key = key;
    s.exchange = cat == 'Q' ? Exchange::NasdaqGS : cat == 'G' ? Exchange::NasdaqGM : cat == 'S' ? Exchange::NasdaqCM
                 : cat == 'N' ? Exchange::NYSE : cat == 'A' ? Exchange::NYSEAmerican : cat == 'P' ? Exchange::NYSEArca
                 : cat == 'Z' ? Exchange::CboeBZX : cat == 'V' ? Exchange::IEX : Exchange::Unknown;
    s.financial_status = fin == ' ' ? 'N' : fin;
    s.round_lot = round_lot;
    symbols->add(std::move(s));
    ++added;
  }
  void on_trading_action(uint16_t, uint64_t, uint64_t, char, const char*) {}
  void on_reg_sho(uint16_t, uint64_t, char) {}
  void on_add(uint16_t, uint64_t, uint64_t, char, uint32_t, uint32_t) { done = true; }
  void on_executed(uint16_t, uint64_t, uint64_t, uint32_t) {}
  void on_executed_price(uint16_t, uint64_t, uint64_t, uint32_t, bool, uint32_t) {}
  void on_cancel(uint16_t, uint64_t, uint64_t, uint32_t) {}
  void on_delete(uint16_t, uint64_t, uint64_t) {}
  void on_replace(uint16_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t) {}
  void on_trade(uint16_t, uint64_t, char, uint32_t, uint64_t, uint32_t) { done = true; }
  void on_cross(uint16_t, uint64_t, uint64_t, uint64_t, uint32_t, char) {}
  void on_broken(uint16_t, uint64_t, uint64_t) {}
};

// Returns the number of symbols added, or -1 if the file can't be opened.
inline long add_itch_directory(const std::string& path, SymbolTable& symbols, std::string* err = nullptr) {
  ItchFileReader r;
  if (!r.open(path, err)) return -1;
  ItchDirectoryCollector dc{&symbols};
  std::atomic<bool> stop{false};
  while (!dc.done && r.run(dc, stop, 0.0, 4096)) {
  }
  return static_cast<long>(dc.added);
}

}  // namespace stok
