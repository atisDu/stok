#include "ref/symbols.hpp"

#include <algorithm>

#include "core/ascii.hpp"
#include "core/hash.hpp"
#include "util/file.hpp"
#include "util/json.hpp"
#include "util/time.hpp"

namespace stok {

const char* exchange_name(Exchange e) {
  switch (e) {
    case Exchange::NasdaqGS: return "Nasdaq GS";
    case Exchange::NasdaqGM: return "Nasdaq GM";
    case Exchange::NasdaqCM: return "Nasdaq CM";
    case Exchange::NYSE: return "NYSE";
    case Exchange::NYSEAmerican: return "NYSE American";
    case Exchange::NYSEArca: return "NYSE Arca";
    case Exchange::CboeBZX: return "Cboe BZX";
    case Exchange::IEX: return "IEX";
    case Exchange::OTC: return "OTC";
    default: return "?";
  }
}

namespace {

bool has_word(std::string_view hay, std::string_view word_lower) {
  std::size_t from = 0;
  for (;;) {
    const std::size_t p = ifind(hay, word_lower, from);
    if (p == std::string_view::npos) return false;
    const bool left_ok = p == 0 || !is_alpha(hay[p - 1]);
    const std::size_t e = p + word_lower.size();
    const bool right_ok = e >= hay.size() || !is_alpha(hay[e]);
    if (left_ok && right_ok) return true;
    from = p + 1;
  }
}

// Splits a pipe-delimited table with a header row; calls f(cols) per data row
// where cols are indexed by the header names requested.
template <typename F>
std::size_t for_each_pipe_row(std::string_view text, std::initializer_list<std::string_view> wanted, F&& f) {
  std::vector<int> idx(wanted.size(), -1);
  bool header = true;
  std::size_t rows = 0;
  std::vector<std::string_view> cells;
  for_each_line(text, [&](std::string_view line) {
    if (line.empty()) return;
    cells.clear();
    split(line, '|', [&](std::string_view c) { cells.push_back(trim(c)); });
    if (header) {
      int k = 0;
      for (auto w : wanted) {
        for (std::size_t i = 0; i < cells.size(); ++i)
          if (iequals(cells[i], w)) idx[static_cast<std::size_t>(k)] = static_cast<int>(i);
        ++k;
      }
      header = false;
      return;
    }
    if (istarts_with(line, "File Creation Time")) return;
    std::vector<std::string_view> out(wanted.size());
    for (std::size_t k = 0; k < idx.size(); ++k)
      if (idx[k] >= 0 && static_cast<std::size_t>(idx[k]) < cells.size()) out[k] = cells[static_cast<std::size_t>(idx[k])];
    f(out);
    ++rows;
  });
  return rows;
}

}  // namespace

bool SymbolInfo::is_derivative_security() const {
  return has_word(name, "warrant") || has_word(name, "warrants") || has_word(name, "unit") ||
         has_word(name, "units") || has_word(name, "right") || has_word(name, "rights");
}

std::string SymbolTable::normalize_company_name(std::string_view name) {
  // Nasdaq security names look like "Acme Robotics, Inc. - Common Stock".
  if (const auto dash = name.find(" - "); dash != std::string_view::npos) name = name.substr(0, dash);
  std::vector<std::string> words;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) words.push_back(std::move(cur));
    cur.clear();
  };
  for (char c : name) {
    if (is_alnum(c)) cur.push_back(to_upper(c));
    else if (c == '&') {
      flush();
      words.push_back("AND");
    } else if (c == '\'' || c == '.') {
      // "Macy's" -> MACYS, "U.S." -> US
    } else {
      flush();
    }
  }
  flush();
  static const char* kSuffix[] = {"INC",   "INCORPORATED", "CORP", "CORPORATION", "CO",   "COMPANY", "LTD",
                                  "LIMITED", "PLC",        "LLC",  "LP",          "HOLDINGS", "HOLDING", "GROUP",
                                  "SA",    "NV",           "AG",   "SE",          "ADR",  "ADS",     "THE",
                                  "COMMON", "STOCK",       "SHARES", "ORDINARY",  "CLASS", "A",      "B"};
  auto is_suffix = [&](const std::string& w) {
    for (const char* s : kSuffix)
      if (w == s) return true;
    return false;
  };
  while (words.size() > 1 && is_suffix(words.back())) words.pop_back();
  if (words.size() > 1 && words.front() == "THE") words.erase(words.begin());
  std::string out;
  for (const auto& w : words) {
    if (!out.empty()) out.push_back(' ');
    out += w;
  }
  return out;
}

void SymbolTable::build_name_index() {
  by_name_.clear();
  std::unordered_map<uint64_t, uint32_t> owner_cik;  // name hash -> cik (0 = unknown issuer)
  for (uint32_t id = 0; id < syms_.size(); ++id) {
    const SymbolInfo& s = syms_[id];
    if (!is_exchange_listed(s.exchange) || s.etf || s.test_issue || s.is_derivative_security() || s.name.empty())
      continue;
    const uint32_t primary = s.cik ? primary_for_cik(s.cik) : id;
    if (primary != id) continue;
    const std::string n = normalize_company_name(s.name);
    if (n.size() < 4) continue;
    const uint64_t h = hash_sv(n);
    auto [slot, inserted] = by_name_.try_emplace(h);
    if (inserted) {
      *slot = id;
      owner_cik[h] = s.cik;
    } else if (*slot != kAmbiguous && (owner_cik[h] != s.cik || s.cik == 0)) {
      *slot = kAmbiguous;
    }
  }
}

uint32_t SymbolTable::find_by_name(std::string_view company_name) const {
  const std::string n = normalize_company_name(company_name);
  if (n.size() < 4) return kInvalid;
  const uint32_t* v = by_name_.find(hash_sv(n));
  if (!v || *v == kAmbiguous) return kInvalid;
  return *v;
}

uint64_t SymbolTable::make_key(std::string_view sym) {
  char b[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  for (std::size_t i = 0; i < sym.size() && i < 8; ++i) b[i] = to_upper(sym[i]);
  return load_u64_le(b);
}

std::string SymbolTable::normalize(std::string_view sym) {
  std::string out;
  out.reserve(sym.size());
  for (char c : trim(sym)) {
    if (c == '-' || c == '/' || c == ' ' || c == '.') out.push_back('.');
    else out.push_back(to_upper(c));
  }
  return out;
}

void SymbolTable::index_alias(std::string_view alias, uint32_t id) {
  if (alias.empty()) return;
  const std::string n = normalize(alias);
  by_norm_.insert_or_assign(hash_sv(n), id);
}

uint32_t SymbolTable::upsert(std::string_view ticker) {
  const uint32_t existing = find(ticker);
  if (existing != kInvalid) return existing;
  SymbolInfo s;
  s.ticker = normalize(ticker);
  return add(std::move(s));
}

uint32_t SymbolTable::add(SymbolInfo info) {
  const uint32_t id = static_cast<uint32_t>(syms_.size());
  if (info.key == 0) info.key = make_key(info.ticker);
  syms_.push_back(std::move(info));
  const SymbolInfo& s = syms_.back();
  index_alias(s.ticker, id);
  by_key_.insert_or_assign(s.key, id);
  if (s.cik) by_cik_[s.cik].push_back(id);
  return id;
}

uint32_t SymbolTable::find(std::string_view ticker) const {
  if (ticker.empty()) return kInvalid;
  char buf[32];
  std::size_t n = 0;
  for (char c : ticker) {
    if (n >= sizeof(buf)) return kInvalid;
    buf[n++] = (c == '-' || c == '/' || c == ' ' || c == '.') ? '.' : to_upper(c);
  }
  const uint32_t* v = by_norm_.find(hash_bytes(buf, n));
  return v ? *v : kInvalid;
}

uint32_t SymbolTable::find_key(uint64_t itch_key) const {
  const uint32_t* v = by_key_.find(itch_key);
  return v ? *v : kInvalid;
}

std::span<const uint32_t> SymbolTable::by_cik(uint32_t cik) const {
  auto it = by_cik_.find(cik);
  if (it == by_cik_.end()) return {};
  return {it->second.data(), it->second.size()};
}

uint32_t SymbolTable::primary_for_cik(uint32_t cik) const {
  const auto ids = by_cik(cik);
  uint32_t best = kInvalid;
  for (uint32_t id : ids) {
    const SymbolInfo& s = syms_[id];
    if (best == kInvalid) {
      best = id;
      continue;
    }
    const SymbolInfo& b = syms_[best];
    const bool s_listed = is_exchange_listed(s.exchange), b_listed = is_exchange_listed(b.exchange);
    if (s_listed != b_listed) {
      if (s_listed) best = id;
      continue;
    }
    if (b.is_derivative_security() && !s.is_derivative_security()) best = id;
    else if (b.is_derivative_security() == s.is_derivative_security() && s.ticker.size() < b.ticker.size())
      best = id;
  }
  return best;
}

std::size_t SymbolTable::load_nasdaq_listed(std::string_view text) {
  return for_each_pipe_row(
      text, {"Symbol", "Security Name", "Market Category", "Test Issue", "Financial Status", "Round Lot Size", "ETF"},
      [&](const std::vector<std::string_view>& c) {
        if (c[0].empty()) return;
        const uint32_t id = upsert(c[0]);
        SymbolInfo& s = syms_[id];
        s.name = std::string(c[1]);
        const char cat = c[2].empty() ? ' ' : c[2][0];
        s.exchange = cat == 'Q' ? Exchange::NasdaqGS : cat == 'G' ? Exchange::NasdaqGM : Exchange::NasdaqCM;
        s.test_issue = !c[3].empty() && c[3][0] == 'Y';
        s.financial_status = c[4].empty() ? 'N' : c[4][0];
        if (auto rl = parse_int<uint32_t>(c[5])) s.round_lot = *rl;
        s.etf = !c[6].empty() && c[6][0] == 'Y';
        const uint64_t k = make_key(c[0]);
        s.key = k;
        by_key_.insert_or_assign(k, id);
      });
}

std::size_t SymbolTable::load_other_listed(std::string_view text) {
  return for_each_pipe_row(
      text, {"ACT Symbol", "Security Name", "Exchange", "CQS Symbol", "ETF", "Round Lot Size", "Test Issue", "NASDAQ Symbol"},
      [&](const std::vector<std::string_view>& c) {
        if (c[0].empty()) return;
        const uint32_t id = upsert(c[0]);
        SymbolInfo& s = syms_[id];
        s.name = std::string(c[1]);
        switch (c[2].empty() ? ' ' : c[2][0]) {
          case 'A': s.exchange = Exchange::NYSEAmerican; break;
          case 'N': s.exchange = Exchange::NYSE; break;
          case 'P': s.exchange = Exchange::NYSEArca; break;
          case 'Z': s.exchange = Exchange::CboeBZX; break;
          case 'V': s.exchange = Exchange::IEX; break;
          default: s.exchange = Exchange::Unknown; break;
        }
        s.etf = !c[4].empty() && c[4][0] == 'Y';
        if (auto rl = parse_int<uint32_t>(c[5])) s.round_lot = *rl;
        s.test_issue = !c[6].empty() && c[6][0] == 'Y';
        index_alias(c[3], id);
        index_alias(c[7], id);
        // ITCH uses Nasdaq integrated symbology.
        const std::string_view itch_sym = c[7].empty() ? c[0] : c[7];
        s.key = make_key(itch_sym);
        by_key_.insert_or_assign(s.key, id);
      });
}

std::size_t SymbolTable::load_sec_tickers(std::string_view json) {
  JsonDoc doc;
  if (!doc.parse(json)) return 0;
  const auto* fields = doc.get(doc.root(), "fields");
  const auto* data = doc.get(doc.root(), "data");
  if (!fields || !data) return 0;
  int i_cik = -1, i_name = -1, i_ticker = -1, i_ex = -1, k = 0;
  doc.for_each(*fields, [&](const JsonDoc::Node& f) {
    if (f.raw == "cik") i_cik = k;
    else if (f.raw == "name") i_name = k;
    else if (f.raw == "ticker") i_ticker = k;
    else if (f.raw == "exchange") i_ex = k;
    ++k;
  });
  if (i_cik < 0 || i_ticker < 0) return 0;
  std::size_t applied = 0;
  doc.for_each(*data, [&](const JsonDoc::Node& row) {
    const JsonDoc::Node* cols[8] = {};
    int n = 0;
    doc.for_each(row, [&](const JsonDoc::Node& c) {
      if (n < 8) cols[n++] = &c;
    });
    if (i_ticker >= n || i_cik >= n) return;
    const std::string ticker = JsonDoc::str(cols[i_ticker]);
    const uint32_t cik = static_cast<uint32_t>(JsonDoc::num(cols[i_cik]));
    if (ticker.empty() || cik == 0) return;
    uint32_t id = find(ticker);
    if (id == kInvalid) {
      // Not in the Nasdaq Trader directories: OTC or stale. Keep it so news
      // can still be attributed (and filtered out as non-listed).
      SymbolInfo s;
      s.ticker = normalize(ticker);
      if (i_name >= 0 && i_name < n) s.name = JsonDoc::str(cols[i_name]);
      const std::string ex = i_ex >= 0 && i_ex < n ? JsonDoc::str(cols[i_ex]) : "";
      // The SEC file doesn't give the Nasdaq tier; small caps are mostly on the
      // Capital Market, and the daily directory overrides this when loaded.
      s.exchange = iequals(ex, "OTC") ? Exchange::OTC : iequals(ex, "Nasdaq") ? Exchange::NasdaqCM
                   : iequals(ex, "NYSE") ? Exchange::NYSE : iequals(ex, "CBOE") ? Exchange::CboeBZX : Exchange::Unknown;
      s.cik = cik;
      id = add(std::move(s));
      ++applied;
      return;
    }
    SymbolInfo& s = syms_[id];
    if (s.cik != cik) {
      s.cik = cik;
      by_cik_[cik].push_back(id);
    }
    if (s.name.empty() && i_name >= 0 && i_name < n) s.name = JsonDoc::str(cols[i_name]);
    ++applied;
  });
  return applied;
}

std::size_t SymbolTable::load_fundamentals(std::string_view tsv) {
  std::size_t applied = 0;
  bool header = true;
  for_each_line(tsv, [&](std::string_view line) {
    if (line.empty() || line[0] == '#') return;
    if (header) {
      header = false;
      if (istarts_with(line, "cik")) return;
    }
    std::string_view f[5];
    int n = 0;
    split(line, '\t', [&](std::string_view c) {
      if (n < 5) f[n++] = c;
    });
    if (n < 2) return;
    auto cik = parse_int<uint32_t>(f[0]);
    if (!cik) return;
    const double shares = parse_double(f[1]).value_or(0);
    int y;
    unsigned m, d;
    const int32_t asof = (n > 2 && timeutil::parse_ymd(f[2], y, m, d))
                             ? static_cast<int32_t>(timeutil::days_from_civil(y, m, d))
                             : 0;
    const double flt = n > 3 ? parse_double(f[3]).value_or(0) : 0;
    for (uint32_t id : by_cik(*cik)) {
      SymbolInfo& s = syms_[id];
      if (shares > 0) {
        s.shares_outstanding = shares;
        s.shares_asof_day = asof;
      }
      if (flt > 0) s.public_float_usd = flt;
      ++applied;
    }
  });
  return applied;
}

std::size_t SymbolTable::load_baseline(std::string_view tsv) {
  std::size_t applied = 0;
  for_each_line(tsv, [&](std::string_view line) {
    if (line.empty() || line[0] == '#' || istarts_with(line, "ticker")) return;
    std::string_view f[4];
    int n = 0;
    split(line, '\t', [&](std::string_view c) {
      if (n < 4) f[n++] = c;
    });
    if (n < 3) return;
    const uint32_t id = find(f[0]);
    if (id == kInvalid) return;
    SymbolInfo& s = syms_[id];
    s.prev_close = parse_double(f[1]).value_or(0);
    s.adv20 = parse_double(f[2]).value_or(0);
    s.adv_days = n > 3 ? parse_int<uint32_t>(f[3]).value_or(0) : 0;
    ++applied;
  });
  return applied;
}

std::size_t SymbolTable::load_finra_short_volume(std::string_view text) {
  std::size_t applied = 0;
  for_each_pipe_row(text, {"Symbol", "ShortVolume", "TotalVolume"}, [&](const std::vector<std::string_view>& c) {
    const uint32_t id = find(c[0]);
    if (id == kInvalid) return;
    const double sv = parse_double(c[1]).value_or(0), tv = parse_double(c[2]).value_or(0);
    if (tv > 0) {
      syms_[id].short_volume_ratio = sv / tv;
      ++applied;
    }
  });
  return applied;
}

void SymbolTable::load_dir(const std::string& dir, std::string* report) {
  auto note = [&](const char* what, std::size_t n) {
    if (report) *report += std::string(what) + "=" + std::to_string(n) + " ";
  };
  if (auto t = fileutil::read_file(fileutil::join(dir, "nasdaqlisted.txt"))) note("nasdaqlisted", load_nasdaq_listed(*t));
  if (auto t = fileutil::read_file(fileutil::join(dir, "otherlisted.txt"))) note("otherlisted", load_other_listed(*t));
  if (auto t = fileutil::read_file(fileutil::join(dir, "company_tickers_exchange.json")))
    note("sec_tickers", load_sec_tickers(*t));
  if (auto t = fileutil::read_file(fileutil::join(dir, "fundamentals.tsv"))) note("fundamentals", load_fundamentals(*t));
  if (auto t = fileutil::read_file(fileutil::join(dir, "finra_shvol.txt"))) note("finra_shvol", load_finra_short_volume(*t));
  build_name_index();
}

}  // namespace stok
