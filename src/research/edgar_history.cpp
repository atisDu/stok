#include "research/edgar_history.hpp"

#include <algorithm>
#include <cstdio>

#include "core/ascii.hpp"
#include "core/hash.hpp"
#include "feeds/parsers.hpp"
#include "feeds/tickers.hpp"
#include "ref/filings.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace stok::research {

namespace {

// Text of the <div class="info"> that follows <div class="infoHead">{head}</div>.
std::string_view info_after(std::string_view html, std::string_view head) {
  const std::string key = ">" + std::string(head) + "<";
  const std::size_t h = html.find(key);
  if (h == std::string_view::npos) return {};
  const std::size_t i = html.find("class=\"info\">", h);
  if (i == std::string_view::npos) return {};
  const std::size_t b = i + 13;
  const std::size_t e = html.find("</div>", b);
  if (e == std::string_view::npos) return {};
  return html.substr(b, e - b);
}

struct MasterRow {
  uint32_t cik;
  std::string company, form, filename;
};

std::vector<MasterRow> parse_master(std::string_view text) {
  std::vector<MasterRow> rows;
  bool in_data = false;
  for_each_line(text, [&](std::string_view line) {
    if (!in_data) {
      if (line.size() > 10 && line.find_first_not_of('-') == std::string_view::npos) in_data = true;
      return;
    }
    std::string_view f[5];
    int n = 0;
    split(line, '|', [&](std::string_view c) {
      if (n < 5) f[n++] = c;
    });
    if (n < 5) return;
    auto cik = parse_int<uint32_t>(f[0]);
    if (!cik) return;
    rows.push_back({*cik, std::string(trim(f[1])), std::string(trim(f[2])), std::string(trim(f[4]))});
  });
  return rows;
}

std::string accession(std::string_view fn) {
  const std::size_t slash = fn.rfind('/');
  std::string_view a = slash == std::string_view::npos ? fn : fn.substr(slash + 1);
  if (a.ends_with(".txt")) a.remove_suffix(4);
  return std::string(a);
}

}  // namespace

std::string master_index_url(const std::string& ymd) {
  int y;
  unsigned m, d;
  if (!timeutil::parse_ymd(ymd, y, m, d)) return {};
  char buf[160];
  std::snprintf(buf, sizeof(buf), "https://www.sec.gov/Archives/edgar/daily-index/%04d/QTR%u/master.%04d%02u%02u.idx",
                y, (m - 1) / 3 + 1, y, m, d);
  return buf;
}

std::string filing_index_url(std::string_view fn) {
  // edgar/data/<cik>/<accession>.txt
  const std::size_t slash = fn.rfind('/');
  if (slash == std::string_view::npos || !fn.ends_with(".txt")) return {};
  const std::string_view dir = fn.substr(0, slash);
  const std::string_view acc = fn.substr(slash + 1, fn.size() - slash - 5);
  std::string nodash;
  for (char c : acc)
    if (c != '-') nodash.push_back(c);
  return "https://www.sec.gov/Archives/" + std::string(dir) + "/" + nodash + "/" + std::string(acc) + "-index.htm";
}

std::optional<int64_t> parse_index_accepted(std::string_view html) {
  const std::string_view v = trim(info_after(html, "Accepted"));
  // "YYYY-MM-DD HH:MM:SS" in US Eastern time
  int y;
  unsigned m, d;
  if (v.size() < 19 || !timeutil::parse_ymd(v.substr(0, 10), y, m, d)) return std::nullopt;
  auto hh = parse_int<int>(v.substr(11, 2));
  auto mi = parse_int<int>(v.substr(14, 2));
  auto ss = parse_int<int>(v.substr(17, 2));
  if (!hh || !mi || !ss) return std::nullopt;
  return timeutil::eastern_midnight_ns(y, m, d) + (static_cast<int64_t>(*hh) * 3600 + *mi * 60 + *ss) * 1'000'000'000LL;
}

uint32_t parse_index_items(std::string_view html) {
  std::string text;
  text::html_to_text(info_after(html, "Items"), text);
  return parse_8k_items(text);
}

EdgarDayStats build_edgar_day(const std::string& ymd, const SymbolTable& symbols, const EdgarHistoryOptions& o,
                              const FetchFn& fetch, std::vector<NewsEvent>& out) {
  EdgarDayStats st;
  const auto master = fetch(master_index_url(ymd));
  if (!master) {
    ++st.errors;
    return st;
  }
  const auto rows = parse_master(*master);
  st.index_rows = rows.size();
  ParseScratch scratch;
  TickerHit hits[16];
  const std::size_t first_new = out.size();
  for (const auto& r : rows) {
    const FormClass cls = classify_form(r.form);
    const bool news_form = cls == FormClass::Form8K || cls == FormClass::Form6K;
    const bool dilution = cls == FormClass::Prospectus || cls == FormClass::S1 || cls == FormClass::S3 ||
                          cls == FormClass::Effect;
    if (!news_form && !(o.dilution_forms && dilution)) continue;
    const uint32_t primary = symbols.primary_for_cik(r.cik);
    const bool listed = primary != SymbolTable::kInvalid && is_exchange_listed(symbols[primary].exchange);
    if (o.listed_only && !listed) continue;
    ++st.candidates;
    if (o.max_filings && st.filings >= o.max_filings) break;
    const std::string index_url = filing_index_url(r.filename);
    const auto index = fetch(index_url);
    if (!index) {
      ++st.errors;
      continue;
    }
    const auto accepted = parse_index_accepted(*index);
    if (!accepted) {
      ++st.missing_accepted;  // can't time it: skip rather than guess
      continue;
    }
    NewsEvent ev;
    ev.reset();
    ev.kind = EventKind::Filing;
    ev.source = 0;
    std::snprintf(ev.form, sizeof(ev.form), "%s", r.form.c_str());
    ev.cik = r.cik;
    ev.items_mask = news_form ? parse_index_items(*index) : 0;
    ev.published_ns = *accepted;
    ev.recv_ns = static_cast<uint64_t>(*accepted + o.dissemination_delay_ms * 1'000'000);
    ev.sent_ns = ev.recv_ns;
    ev.parsed_ns = ev.recv_ns;
    ev.id_hash = hash_sv(r.filename);
    std::string title = r.form + " " + r.company;
    bool first = true;
    for (int bit = 0; bit < 31; ++bit) {
      if (!(ev.items_mask & (1u << bit))) continue;
      title += first ? ": " : "; ";
      title += form8k_item_title(bit);
      first = false;
    }
    ev.title.assign(title);
    ev.title_key = text::headline_key(title);
    ev.link.assign(index_url);
    // Same shape as the live Atom summary ("Filed: … AccNo: … Item 1.01: …") so rules score alike.
    std::string body = "Filed: " + ymd + " AccNo: " + accession(r.filename) +
                       " Accepted: " + timeutil::eastern_hms(*accepted);
    if (news_form) {
      std::string items;
      text::html_to_text(info_after(*index, "Items"), items);
      if (!items.empty()) body += " " + items;
    }
    ev.body.assign(body);
    if (primary != SymbolTable::kInvalid) ev.tickers[ev.n_tickers++] = primary;
    for (uint32_t id : symbols.by_cik(r.cik))
      if (id != primary && ev.n_tickers < kMaxTickers) ev.tickers[ev.n_tickers++] = id;
    out.push_back(ev);
    ++st.filings;

    if (!news_form || !o.fetch_exhibits) continue;
    const std::string href = find_exhibit_href(*index);
    if (href.empty()) continue;
    std::string ex_url = href;
    if (!istarts_with(ex_url, "http")) ex_url = "https://www.sec.gov" + (href[0] == '/' ? href : "/" + href);
    const auto ex = fetch(ex_url);
    if (!ex) {
      ++st.errors;
      continue;
    }
    NewsEvent doc;
    fill_exhibit_event(*ex, ev, ex_url, doc, scratch);
    doc.source = 1;
    doc.recv_ns = ev.recv_ns + static_cast<uint64_t>(o.exhibit_delay_ms) * 1'000'000;
    doc.sent_ns = doc.recv_ns;
    doc.parsed_ns = doc.recv_ns;
    if (doc.n_tickers == 0) {
      // Issuer not in today's CIK map (delisted/renamed): use the release's own tags.
      const int n = extract_tickers(doc.body.view(), symbols, hits, 16);
      for (int k = 0; k < n && doc.n_tickers < kMaxTickers; ++k)
        if (hits[k].sym != SymbolTable::kInvalid) doc.tickers[doc.n_tickers++] = hits[k].sym;
    }
    out.push_back(doc);
    ++st.exhibits;
  }
  std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first_new), out.end(),
                   [](const NewsEvent& a, const NewsEvent& b) { return a.recv_ns < b.recv_ns; });
  return st;
}

}  // namespace stok::research
