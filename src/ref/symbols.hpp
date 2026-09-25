#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/flat_hash.hpp"

namespace stok {

enum class Exchange : uint8_t {
  Unknown = 0,
  NasdaqGS,
  NasdaqGM,
  NasdaqCM,
  NYSE,
  NYSEAmerican,
  NYSEArca,
  CboeBZX,
  IEX,
  OTC,
};

const char* exchange_name(Exchange e);
inline bool is_exchange_listed(Exchange e) { return e != Exchange::Unknown && e != Exchange::OTC; }

struct SymbolInfo {
  std::string ticker;  // display form, e.g. "BRK.B"
  uint64_t key = 0;    // ITCH 8-byte space-padded stock field
  std::string name;
  Exchange exchange = Exchange::Unknown;
  char financial_status = 'N';  // Nasdaq: N normal, D deficient, E delinquent, Q bankrupt, G/H/J/K combos
  bool etf = false;
  bool test_issue = false;
  uint32_t round_lot = 100;
  uint32_t cik = 0;
  // --- official fundamentals (SEC XBRL) ---
  double shares_outstanding = 0;   // dei:EntityCommonStockSharesOutstanding
  int32_t shares_asof_day = 0;     // days since epoch
  double public_float_usd = 0;     // dei:EntityPublicFloat
  // --- own baseline (built from the market feed) ---
  double prev_close = 0;
  double adv20 = 0;       // average daily volume, same venue(s) as the live feed
  uint32_t adv_days = 0;
  // --- FINRA Reg SHO daily short volume ---
  double short_volume_ratio = -1;

  // Warrants, units and rights (secondary securities of an issuer).
  bool is_derivative_security() const;
  bool distressed() const { return financial_status != 'N' && financial_status != ' ' && financial_status != '\0'; }
};

// Security master built from official sources: Nasdaq Trader symbol
// directories (every exchange-listed US security), the SEC ticker/CIK map,
// SEC XBRL share counts, and the daemon's own volume baseline. Built once at
// start-up and read-only afterwards, so all threads share it without locks.
class SymbolTable {
 public:
  static constexpr uint32_t kInvalid = UINT32_MAX;

  // Loaders return the number of rows applied.
  std::size_t load_nasdaq_listed(std::string_view text);  // nasdaqlisted.txt
  std::size_t load_other_listed(std::string_view text);   // otherlisted.txt
  std::size_t load_sec_tickers(std::string_view json);    // company_tickers_exchange.json
  std::size_t load_fundamentals(std::string_view tsv);    // written by stok-ref
  std::size_t load_baseline(std::string_view tsv);        // written by the daemon / stok-replay
  std::size_t load_finra_short_volume(std::string_view text);

  // Loads every known file present in `dir`. Appends a summary to `report`.
  void load_dir(const std::string& dir, std::string* report);

  uint32_t add(SymbolInfo info);

  uint32_t find(std::string_view ticker) const;
  uint32_t find_key(uint64_t itch_key) const;
  std::span<const uint32_t> by_cik(uint32_t cik) const;
  // The issuer's primary security (common stock over warrants/units/rights).
  uint32_t primary_for_cik(uint32_t cik) const;

  const SymbolInfo& operator[](uint32_t id) const { return syms_[id]; }
  SymbolInfo& mut(uint32_t id) { return syms_[id]; }
  std::size_t size() const { return syms_.size(); }

  // Issuer-name index for stories without an exchange tag. Built from SEC
  // company names and Nasdaq security names, exchange-listed primary
  // securities only; names shared by two issuers are marked ambiguous.
  void build_name_index();
  uint32_t find_by_name(std::string_view company_name) const;
  // "Acme Robotics, Inc." -> "ACME ROBOTICS" (suffixes like Inc/Corp/Ltd dropped).
  static std::string normalize_company_name(std::string_view name);

  // 8 bytes, left-justified, space padded: the ITCH "Stock" field.
  static uint64_t make_key(std::string_view sym);
  // Uppercase; class/series separators ('-', '/', ' ', '.') unified to '.'.
  static std::string normalize(std::string_view sym);

 private:
  uint32_t upsert(std::string_view ticker);
  void index_alias(std::string_view alias, uint32_t id);

  std::vector<SymbolInfo> syms_;
  FlatMap64<uint32_t> by_norm_{1024};  // hash(normalized) -> id
  FlatMap64<uint32_t> by_key_{1024};   // ITCH key -> id
  std::unordered_map<uint32_t, std::vector<uint32_t>> by_cik_;
  FlatMap64<uint32_t> by_name_{1024};  // hash(normalized name) -> id (kAmbiguous if shared)
  static constexpr uint32_t kAmbiguous = UINT32_MAX - 1;
};

}  // namespace stok
