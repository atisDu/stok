#include "feeds/parsers.hpp"

#include <cstdio>
#include <cstring>

#include "ref/filings.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace stok {

FeedKind parse_feed_kind(std::string_view s, bool* ok) {
  if (ok) *ok = true;
  if (iequals(s, "rss") || iequals(s, "atom") || iequals(s, "wire")) return FeedKind::Rss;
  if (iequals(s, "edgar")) return FeedKind::EdgarAtom;
  if (iequals(s, "halts") || iequals(s, "nasdaq_halts")) return FeedKind::NasdaqHalts;
  if (ok) *ok = false;
  return FeedKind::Rss;
}

const char* feed_kind_name(FeedKind k) {
  switch (k) {
    case FeedKind::Rss: return "rss";
    case FeedKind::EdgarAtom: return "edgar";
    case FeedKind::NasdaqHalts: return "halts";
  }
  return "?";
}

namespace {

// Raw XML text -> plain text (CDATA / entities / embedded HTML).
void text_of(std::string_view raw, std::string& tmp, std::string& out, std::size_t max_out = SIZE_MAX,
             char block_sep = ' ') {
  out.clear();
  bool cdata = false;
  const std::string_view v = xml::unwrap_cdata(raw, &cdata);
  if (cdata) {
    text::html_to_text(v, out, max_out, block_sep);
    return;
  }
  tmp.clear();
  text::append_decoded_entities(v, tmp);
  text::html_to_text(tmp, out, max_out, block_sep);
}

// Attribute values in XML are entity-encoded (&amp; in URLs).
void decode_attr(std::string_view raw, std::string& out) {
  out.clear();
  text::append_decoded_entities(raw, out);
}

void add_tickers_from_hits(NewsEvent& ev, const TickerHit* hits, int n) {
  for (int k = 0; k < n; ++k) {
    const TickerHit& h = hits[k];
    if (h.sym != SymbolTable::kInvalid) {
      bool dup = false;
      for (int j = 0; j < ev.n_tickers; ++j) dup |= ev.tickers[j] == h.sym;
      if (!dup && ev.n_tickers < kMaxTickers) ev.tickers[ev.n_tickers++] = h.sym;
    } else if (ev.n_unresolved < kMaxUnresolved) {
      std::memcpy(ev.unresolved[ev.n_unresolved], h.raw, sizeof(ev.unresolved[0]));
      ev.unresolved[ev.n_unresolved][sizeof(ev.unresolved[0]) - 1] = '\0';
      ++ev.n_unresolved;
    }
  }
}

int64_t item_date(std::string_view item) {
  for (std::string_view tag : {"pubDate", "dc:date", "published", "updated", "a10:updated"}) {
    const std::string_view v = trim(xml::child_text(item, tag));
    if (!v.empty()) {
      if (auto t = timeutil::parse_any_datetime(v)) return *t;
    }
  }
  return 0;
}

bool fill_rss(std::string_view item, const SymbolTable& symbols, NewsEvent& ev, ParseScratch& s) {
  ev.kind = EventKind::News;
  text_of(xml::child_text(item, "title"), s.a, s.b, 1024);
  if (s.b.empty()) return false;
  ev.title.assign(s.b);
  ev.title_key = text::headline_key(s.b);

  // Link: RSS <link>text</link>, or Atom <link rel="alternate" href="..."/>.
  std::string_view link = trim(xml::child_text(item, "link"));
  if (link.empty()) {
    xml::for_each_element(item, "link", [&](const xml::Element& e) {
      const std::string_view rel = xml::attr_value(e.attrs, "rel");
      if (link.empty() || rel == "alternate") link = xml::attr_value(e.attrs, "href");
    });
  }
  decode_attr(xml::unwrap_cdata(link), s.a);
  ev.link.assign(trim(s.a));
  ev.published_ns = item_date(item);

  // Body: first of description / content:encoded / summary / content.
  std::string_view body_raw;
  for (std::string_view tag : {"description", "content:encoded", "summary", "content"}) {
    body_raw = xml::child_text(item, tag);
    if (!trim(body_raw).empty()) break;
  }
  text_of(body_raw, s.a, s.text, decltype(ev.body)::capacity() + 512);
  ev.body.assign(s.text);

  // Categories (GlobeNewswire puts "Nasdaq:ABCD" there) + text -> tickers.
  s.cats.clear();
  xml::for_each_element(item, "category", [&](const xml::Element& e) {
    std::string_view v = trim(xml::unwrap_cdata(e.inner));
    if (v.empty()) v = xml::attr_value(e.attrs, "term");
    if (v.empty()) return;
    s.cats += " | ";
    text::append_decoded_entities(v, s.cats);
  });
  const std::string_view heads[] = {std::string_view(s.cats), ev.title.view(), std::string_view(s.text)};
  for (std::string_view h : heads) {
    const int n = extract_tickers(h, symbols, s.hits, 16);
    add_tickers_from_hits(ev, s.hits, n);
  }
  return true;
}

bool fill_edgar(std::string_view entry, const SymbolTable& symbols, NewsEvent& ev, ParseScratch& s) {
  ev.kind = EventKind::Filing;
  text_of(xml::child_text(entry, "title"), s.a, s.c, 512);
  EdgarTitle t;
  if (!parse_edgar_title(s.c, t)) return false;
  std::snprintf(ev.form, sizeof(ev.form), "%.*s", static_cast<int>(t.form.size()), t.form.data());
  ev.cik = t.cik;

  xml::for_each_element(entry, "link", [&](const xml::Element& e) {
    if (ev.link.empty()) {
      decode_attr(xml::attr_value(e.attrs, "href"), s.a);
      ev.link.assign(s.a);
    }
  });
  if (const std::string_view up = trim(xml::child_text(entry, "updated")); !up.empty()) {
    if (auto ts = timeutil::parse_iso8601(up)) ev.published_ns = *ts;
  }
  // Summary: "Filed: 2026-09-25 AccNo: ... Size: 245 KB Item 1.01: Entry into ..."
  text_of(xml::child_text(entry, "summary"), s.a, s.text, 4096);
  ev.items_mask = parse_8k_items(s.text);
  ev.body.assign(s.text);

  // Title: "8-K ACME CORP: Entry into a Material Definitive Agreement; ..."
  s.b.clear();
  s.b.append(t.form);
  s.b.push_back(' ');
  s.b.append(t.company);
  bool first = true;
  for (int bit = 0; bit < 32; ++bit) {
    if (!(ev.items_mask & (1u << bit)) || bit == 31) continue;  // skip 9.01 exhibits
    s.b.append(first ? ": " : "; ");
    s.b.append(form8k_item_title(bit));
    first = false;
  }
  ev.title.assign(s.b);
  ev.title_key = text::headline_key(s.b);

  // Issuer securities, primary first.
  const uint32_t primary = symbols.primary_for_cik(t.cik);
  if (primary != SymbolTable::kInvalid) ev.tickers[ev.n_tickers++] = primary;
  for (uint32_t id : symbols.by_cik(t.cik)) {
    if (id == primary || ev.n_tickers >= kMaxTickers) continue;
    ev.tickers[ev.n_tickers++] = id;
  }
  return true;
}

bool fill_halt(std::string_view item, const SymbolTable& symbols, NewsEvent& ev, ParseScratch& s) {
  ev.kind = EventKind::Halt;
  const std::string_view sym = trim(xml::child_text(item, "ndaq:IssueSymbol"));
  if (sym.empty()) return false;
  const std::string_view reason = trim(xml::child_text(item, "ndaq:ReasonCode"));
  const std::string_view market = trim(xml::child_text(item, "ndaq:Market"));
  std::snprintf(ev.halt.reason, sizeof(ev.halt.reason), "%.*s", static_cast<int>(reason.size()), reason.data());
  std::snprintf(ev.halt.market, sizeof(ev.halt.market), "%.*s", static_cast<int>(market.size()), market.data());
  const auto halt_ts = timeutil::parse_eastern_mdy_time(xml::child_text(item, "ndaq:HaltDate"),
                                                        xml::child_text(item, "ndaq:HaltTime"));
  ev.halt.halt_ns = halt_ts.value_or(0);
  const std::string_view rdate = trim(xml::child_text(item, "ndaq:ResumptionDate"));
  const std::string_view rq = trim(xml::child_text(item, "ndaq:ResumptionQuoteTime"));
  const std::string_view rt = trim(xml::child_text(item, "ndaq:ResumptionTradeTime"));
  if (!rdate.empty() && !rq.empty()) ev.halt.resume_quote_ns = timeutil::parse_eastern_mdy_time(rdate, rq).value_or(0);
  if (!rdate.empty() && !rt.empty()) ev.halt.resume_trade_ns = timeutil::parse_eastern_mdy_time(rdate, rt).value_or(0);
  ev.halt.pause_threshold = parse_double(xml::child_text(item, "ndaq:PauseThresholdPrice")).value_or(0.0);
  ev.published_ns = ev.halt.halt_ns ? ev.halt.halt_ns : item_date(item);

  const uint32_t id = symbols.find(sym);
  if (id != SymbolTable::kInvalid) {
    ev.tickers[ev.n_tickers++] = id;
  } else {
    std::snprintf(ev.unresolved[0], sizeof(ev.unresolved[0]), "%.*s", static_cast<int>(sym.size()), sym.data());
    ev.n_unresolved = 1;
  }
  text_of(xml::child_text(item, "ndaq:IssueName"), s.a, s.b, 200);
  char title[256];
  if (ev.halt.resume_trade_ns) {
    std::snprintf(title, sizeof(title), "RESUME %.*s %s (%s: %s) trading %s ET", static_cast<int>(sym.size()),
                  sym.data(), s.b.c_str(), ev.halt.reason, halt_reason_desc(ev.halt.reason),
                  timeutil::eastern_hms(ev.halt.resume_trade_ns).c_str());
  } else {
    std::snprintf(title, sizeof(title), "HALT %.*s %s (%s: %s) at %s ET", static_cast<int>(sym.size()), sym.data(),
                  s.b.c_str(), ev.halt.reason, halt_reason_desc(ev.halt.reason),
                  ev.halt.halt_ns ? timeutil::eastern_hms(ev.halt.halt_ns).c_str() : "?");
  }
  ev.title.assign(title);
  ev.title_key = text::headline_key(title);
  return true;
}

}  // namespace

bool fill_event(FeedKind kind, std::string_view item, const SymbolTable& symbols, NewsEvent& ev, ParseScratch& s) {
  switch (kind) {
    case FeedKind::Rss: return fill_rss(item, symbols, ev, s);
    case FeedKind::EdgarAtom: return fill_edgar(item, symbols, ev, s);
    case FeedKind::NasdaqHalts: return fill_halt(item, symbols, ev, s);
  }
  return false;
}

bool parse_edgar_title(std::string_view title, EdgarTitle& out) {
  title = trim(title);
  const std::size_t dash = title.find(" - ");
  if (dash == std::string_view::npos) return false;
  out.form = trim(title.substr(0, dash));
  std::string_view rest = title.substr(dash + 3);
  // Find "(##########)" -- the CIK in parentheses.
  std::size_t search = 0;
  for (;;) {
    const std::size_t lp = rest.find('(', search);
    if (lp == std::string_view::npos) return false;
    const std::size_t rp = rest.find(')', lp);
    if (rp == std::string_view::npos) return false;
    const std::string_view inner = rest.substr(lp + 1, rp - lp - 1);
    bool digits = !inner.empty() && inner.size() <= 10;
    for (char c : inner) digits &= is_digit(c);
    if (digits) {
      out.cik = parse_int<uint32_t>(inner).value_or(0);
      out.company = trim(rest.substr(0, lp));
      const std::size_t lp2 = rest.find('(', rp);
      const std::size_t rp2 = lp2 == std::string_view::npos ? lp2 : rest.find(')', lp2);
      if (rp2 != std::string_view::npos) out.role = rest.substr(lp2 + 1, rp2 - lp2 - 1);
      return out.cik != 0 && !out.form.empty();
    }
    search = rp + 1;
  }
}

uint32_t parse_8k_items(std::string_view text) {
  uint32_t mask = 0;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t p = ifind(text, "item ", pos);
    if (p == std::string_view::npos) return mask;
    std::size_t k = p + 5;
    if (k + 3 < text.size() && is_digit(text[k]) && text[k + 1] == '.' && is_digit(text[k + 2]) &&
        is_digit(text[k + 3])) {
      const int major = text[k] - '0';
      const int minor = (text[k + 2] - '0') * 10 + (text[k + 3] - '0');
      const int bit = form8k_item_bit(major, minor);
      if (bit >= 0) mask |= 1u << bit;
    }
    pos = p + 5;
  }
}

std::string find_exhibit_href(std::string_view html) {
  std::size_t pos = 0;
  std::string best;
  int best_rank = 99;
  for (;;) {
    const std::size_t tr = ifind(html, "<tr", pos);
    if (tr == std::string_view::npos) break;
    std::size_t end = ifind(html, "</tr", tr + 3);
    if (end == std::string_view::npos) end = html.size();
    const std::string_view row = html.substr(tr, end - tr);
    pos = end;
    const std::size_t ex = ifind(row, ">ex-99");
    if (ex == std::string_view::npos) continue;
    // Prefer EX-99.1 (the press release) over later exhibits.
    int rank = 5;
    if (ex + 8 < row.size() && row[ex + 6] == '.' && row[ex + 7] == '1' && !is_digit(row[ex + 8])) rank = 0;
    const std::size_t a = ifind(row, "href=");
    if (a == std::string_view::npos) continue;
    std::size_t q = a + 5;
    if (q >= row.size()) continue;
    const char quote = row[q];
    std::string_view href;
    if (quote == '"' || quote == '\'') {
      const std::size_t qe = row.find(quote, q + 1);
      if (qe == std::string_view::npos) continue;
      href = row.substr(q + 1, qe - q - 1);
    } else {
      std::size_t qe = q;
      while (qe < row.size() && !is_space(row[qe]) && row[qe] != '>') ++qe;
      href = row.substr(q, qe - q);
    }
    if (istarts_with(href, "/ix?doc=")) href.remove_prefix(8);
    if (rank < best_rank) {
      best_rank = rank;
      best.assign(href);
      if (rank == 0) break;
    }
  }
  return best;
}

void fill_exhibit_event(std::string_view html, const NewsEvent& filing, std::string_view doc_url, NewsEvent& ev,
                        ParseScratch& s) {
  ev = filing;
  ev.kind = EventKind::FilingDoc;
  ev.link.assign(doc_url);
  ev.id_hash = hash_sv(doc_url);
  s.text.clear();
  text::html_to_text(html, s.text, 16 * 1024, '\n');
  // Headline: first substantial line that isn't exhibit boilerplate.
  std::string_view headline;
  std::size_t body_from = 0;
  for_each_line(s.text, [&](std::string_view line) {
    if (!headline.empty()) return;
    const std::string_view t = trim(line);
    body_from += line.size() + 1;
    if (t.size() < 15) return;
    if (istarts_with(t, "exhibit") || istarts_with(t, "ex-99") || istarts_with(t, "for immediate release") ||
        istarts_with(t, "press release") || istarts_with(t, "news release"))
      return;
    headline = t.substr(0, 250);
  });
  if (headline.empty()) headline = trim(std::string_view(s.text).substr(0, 200));
  ev.title.assign(headline);
  ev.title_key = text::headline_key(ev.title.view());
  s.b.assign(s.text);
  for (auto& c : s.b)
    if (c == '\n') c = ' ';
  ev.body.assign(s.b);
}

const char* halt_reason_desc(std::string_view c) {
  struct R {
    const char* code;
    const char* desc;
  };
  static const R kReasons[] = {
      {"T1", "News pending"},
      {"T2", "News released"},
      {"T3", "News and resumption times"},
      {"T5", "Single-stock trading pause"},
      {"T6", "Extraordinary market activity"},
      {"T7", "Quotation-only period"},
      {"T8", "ETF halt"},
      {"T12", "Additional information requested"},
      {"H4", "Non-compliance"},
      {"H9", "Not current"},
      {"H10", "SEC trading suspension"},
      {"H11", "Regulatory concern"},
      {"O1", "Operations halt"},
      {"IPO1", "IPO not yet trading"},
      {"IPOQ", "IPO released for quotation"},
      {"IPOE", "IPO positioning window extension"},
      {"M1", "Corporate action"},
      {"M2", "Quotation not available"},
      {"LUDP", "Volatility trading pause (LULD)"},
      {"LUDS", "Volatility trading pause, straddle"},
      {"MWC1", "Market-wide circuit breaker L1"},
      {"MWC2", "Market-wide circuit breaker L2"},
      {"MWC3", "Market-wide circuit breaker L3"},
      {"MWC0", "Market-wide circuit breaker carryover"},
      {"R4", "Qualification issues resolved"},
      {"R9", "Filing requirements satisfied"},
      {"C3", "Issuer news not forthcoming"},
      {"C4", "Qualifications halt ended"},
      {"C9", "Qualifications halt concluded"},
      {"C11", "Halt concluded by other authority"},
      {"R1", "New issue available"},
      {"D", "Security deletion"},
  };
  c = trim(c);
  for (const auto& r : kReasons)
    if (iequals(c, r.code)) return r.desc;
  return "Other";
}

}  // namespace stok
