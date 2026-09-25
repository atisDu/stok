#pragma once

#include <cstdint>
#include <string_view>

#include "ref/symbols.hpp"

namespace stok {

enum class VenueClass : uint8_t { US = 0, OTC = 1, Foreign = 2 };

struct TickerHit {
  uint32_t sym = SymbolTable::kInvalid;  // resolved symbol id (US or OTC), else kInvalid
  VenueClass venue = VenueClass::US;
  char raw[16] = {};                     // "OTC:ABCDF", "TSXV:XYZ", or the bare US ticker
};

// Finds exchange-tagged tickers in press-release text:
//   "(NASDAQ: ABCD)", "(Nasdaq:ABCD, ABCDW)", "NYSE American: XYZ",
//   "(OTCQB: ABCDF)", "(TSXV: ABC) (OTCQX: ABCDF)", "Nasdaq:ABCD" (categories)
// Works by locating ':' (rare in prose) and checking the preceding words
// against a venue table. Hits are deduplicated. Returns the count written.
int extract_tickers(std::string_view text, const SymbolTable& symbols, TickerHit* out, int max_out);

}  // namespace stok
