#include "feeds/tickers.hpp"

#include <cstdio>
#include <cstring>

#include "core/ascii.hpp"

namespace stok {

namespace {

struct Venue {
  const char* name;  // lowercase, as written before the colon
  VenueClass cls;
  const char* tag;   // prefix used for unresolved hits
};

// Longest names first so "nyse american" wins over "nyse".
constexpr Venue kVenues[] = {
    {"nasdaq global select market", VenueClass::US, "NASDAQ"},
    {"nasdaq global market", VenueClass::US, "NASDAQ"},
    {"nasdaq capital market", VenueClass::US, "NASDAQ"},
    {"nyse american llc", VenueClass::US, "NYSE"},
    {"nyse american", VenueClass::US, "NYSE"},
    {"nyse mkt", VenueClass::US, "NYSE"},
    {"nyse arca", VenueClass::US, "NYSE"},
    {"nysearca", VenueClass::US, "NYSE"},
    {"nyseamerican", VenueClass::US, "NYSE"},
    {"nasdaqgs", VenueClass::US, "NASDAQ"},
    {"nasdaqgm", VenueClass::US, "NASDAQ"},
    {"nasdaqcm", VenueClass::US, "NASDAQ"},
    {"nasdaq", VenueClass::US, "NASDAQ"},
    {"nyse", VenueClass::US, "NYSE"},
    {"amex", VenueClass::US, "NYSE"},
    {"cboe bzx", VenueClass::US, "CBOE"},
    {"cboe", VenueClass::US, "CBOE"},
    {"bats", VenueClass::US, "CBOE"},
    {"otc pink", VenueClass::OTC, "OTC"},
    {"otc markets", VenueClass::OTC, "OTC"},
    {"otcmkts", VenueClass::OTC, "OTC"},
    {"otcqx", VenueClass::OTC, "OTC"},
    {"otcqb", VenueClass::OTC, "OTC"},
    {"otc", VenueClass::OTC, "OTC"},
    {"pink", VenueClass::OTC, "OTC"},
    {"tsx venture", VenueClass::Foreign, "TSXV"},
    {"tsx-v", VenueClass::Foreign, "TSXV"},
    {"tsxv", VenueClass::Foreign, "TSXV"},
    {"tsx", VenueClass::Foreign, "TSX"},
    {"cse", VenueClass::Foreign, "CSE"},
    {"neo", VenueClass::Foreign, "NEO"},
    {"cboe canada", VenueClass::Foreign, "NEO"},
    {"asx", VenueClass::Foreign, "ASX"},
    {"lse", VenueClass::Foreign, "LSE"},
    {"aim", VenueClass::Foreign, "AIM"},
    {"fse", VenueClass::Foreign, "FSE"},
    {"frankfurt", VenueClass::Foreign, "FSE"},
    {"xetra", VenueClass::Foreign, "FSE"},
    {"hkex", VenueClass::Foreign, "HKEX"},
    {"sgx", VenueClass::Foreign, "SGX"},
    {"tase", VenueClass::Foreign, "TASE"},
};

bool boundary_before(std::string_view text, std::size_t pos) {
  if (pos == 0) return true;
  const char c = text[pos - 1];
  return c == '(' || c == '[' || c == ' ' || c == ',' || c == ';' || c == '/' || c == '\n' || c == '\t' ||
         c == '|' || c == '>' || c == '"';
}

// Matches a venue name ending right before `colon` (ignoring spaces).
const Venue* venue_before(std::string_view text, std::size_t colon) {
  std::size_t end = colon;
  while (end > 0 && text[end - 1] == ' ') --end;
  for (const Venue& v : kVenues) {
    const std::size_t n = std::strlen(v.name);
    if (n > end) continue;
    const std::size_t start = end - n;
    bool eq = true;
    for (std::size_t k = 0; k < n && eq; ++k) eq = to_lower(text[start + k]) == v.name[k];
    if (eq && boundary_before(text, start)) return &v;
  }
  return nullptr;
}

// Reads one ticker token at `i`: [A-Z][A-Z0-9.\-]{0,9}, must not be followed
// by a lowercase letter (to reject prose like "Nasdaq: The ...").
std::size_t read_ticker(std::string_view text, std::size_t i, std::string_view& tok) {
  const std::size_t b = i;
  if (i >= text.size() || !is_upper(text[i])) return 0;
  ++i;
  while (i < text.size() && i - b < 10 && (is_upper(text[i]) || is_digit(text[i]) || text[i] == '.' || text[i] == '-'))
    ++i;
  // Trailing punctuation belongs to the sentence, not the ticker.
  while (i > b + 1 && (text[i - 1] == '.' || text[i - 1] == '-')) --i;
  if (i < text.size() && (text[i] >= 'a' && text[i] <= 'z')) return 0;
  tok = text.substr(b, i - b);
  return i - b;
}

}  // namespace

int extract_tickers(std::string_view text, const SymbolTable& symbols, TickerHit* out, int max_out) {
  int n = 0;
  auto push = [&](uint32_t sym, VenueClass cls, std::string_view tag, std::string_view tok) {
    if (n >= max_out) return;
    char raw[16];
    if (cls == VenueClass::US) std::snprintf(raw, sizeof(raw), "%.*s", static_cast<int>(tok.size()), tok.data());
    else std::snprintf(raw, sizeof(raw), "%.*s:%.*s", static_cast<int>(tag.size()), tag.data(), static_cast<int>(tok.size()), tok.data());
    for (int k = 0; k < n; ++k) {
      if (sym != SymbolTable::kInvalid && out[k].sym == sym) return;
      if (std::strcmp(out[k].raw, raw) == 0) return;
    }
    out[n].sym = sym;
    out[n].venue = cls;
    std::memcpy(out[n].raw, raw, sizeof(raw));
    ++n;
  };

  std::size_t pos = 0;
  while (pos < text.size() && n < max_out) {
    const void* p = std::memchr(text.data() + pos, ':', text.size() - pos);
    if (!p) break;
    const std::size_t colon = static_cast<std::size_t>(static_cast<const char*>(p) - text.data());
    pos = colon + 1;
    const Venue* v = venue_before(text, colon);
    if (!v) continue;
    std::size_t i = colon + 1;
    for (;;) {
      while (i < text.size() && text[i] == ' ') ++i;
      std::string_view tok;
      const std::size_t len = read_ticker(text, i, tok);
      if (!len) break;
      // Foreign tickers are informational only. A US tag whose symbol isn't in
      // the security master (typo or brand-new listing) stays unresolved.
      const uint32_t sym = v->cls == VenueClass::Foreign ? SymbolTable::kInvalid : symbols.find(tok);
      push(sym, v->cls, v->tag, tok);
      i += len;
      // "(NASDAQ: ABCD, ABCDW)" / "(NYSE: A and B)" continuation.
      std::size_t j = i;
      while (j < text.size() && text[j] == ' ') ++j;
      if (j < text.size() && text[j] == ',') {
        i = j + 1;
        continue;
      }
      if (j + 4 < text.size() && text.compare(j, 4, "and ") == 0) {
        i = j + 4;
        continue;
      }
      break;
    }
    pos = i > pos ? i : pos;
  }
  return n;
}

}  // namespace stok
