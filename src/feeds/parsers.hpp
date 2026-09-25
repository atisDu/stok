#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/hash.hpp"
#include "engine/events.hpp"
#include "feeds/tickers.hpp"
#include "feeds/xml.hpp"
#include "ref/symbols.hpp"

namespace stok {

enum class FeedKind : uint8_t {
  Rss = 0,          // generic RSS 2.0 / Atom wire feed
  EdgarAtom = 1,    // SEC EDGAR "getcurrent" Atom feed
  NasdaqHalts = 2,  // Nasdaq Trader trade-halts RSS
};

FeedKind parse_feed_kind(std::string_view s, bool* ok = nullptr);
const char* feed_kind_name(FeedKind k);

// Reused per-thread buffers so parsing allocates nothing in steady state.
struct ParseScratch {
  std::string a, b, c, cats, text;
  TickerHit hits[16];
};

// Pass 1 (cheap): iterates items and computes each one's dedupe id. The
// caller skips items it has already seen, so only new items get parsed fully.
template <typename F>
void scan_feed(FeedKind kind, std::string_view doc, F&& f) {
  auto id_of = [&](std::string_view item) -> uint64_t {
    if (kind == FeedKind::NasdaqHalts) {
      // Halt items are updated in place when resumption times are set, so the
      // id covers the mutable fields: an update is a new event.
      uint64_t h = 0x51ed27;
      for (std::string_view tag : {"ndaq:IssueSymbol", "ndaq:HaltDate", "ndaq:HaltTime", "ndaq:ReasonCode",
                                   "ndaq:ResumptionDate", "ndaq:ResumptionQuoteTime", "ndaq:ResumptionTradeTime"}) {
        const std::string_view v = trim(xml::child_text(item, tag));
        h = hash_bytes(v.data(), v.size(), h);
      }
      return h;
    }
    std::string_view id = trim(xml::child_text(item, "guid"));
    if (id.empty()) id = trim(xml::child_text(item, "id"));
    if (id.empty()) id = trim(xml::child_text(item, "link"));
    if (id.empty()) id = trim(xml::child_text(item, "title"));
    return hash_sv(id);
  };
  bool any = false;
  if (kind != FeedKind::EdgarAtom) {
    xml::for_each_item(doc, "item", [&](std::string_view item, std::string_view) {
      any = true;
      f(item, id_of(item));
    });
  }
  if (!any) {
    xml::for_each_item(doc, "entry", [&](std::string_view item, std::string_view) { f(item, id_of(item)); });
  }
}

// Pass 2: fills the content fields of `ev` (kind, title, link, body, dates,
// tickers, EDGAR fields). Transport fields (times, source) are set by the
// caller. Returns false if the item has no usable content.
bool fill_event(FeedKind kind, std::string_view item, const SymbolTable& symbols, NewsEvent& ev, ParseScratch& s);

// ---- EDGAR helpers ----
struct EdgarTitle {
  std::string_view form;
  std::string_view company;
  uint32_t cik = 0;
  std::string_view role;  // Filer / Subject / Filed by
};
// "8-K - ACME CORP (0001234567) (Filer)"
bool parse_edgar_title(std::string_view title, EdgarTitle& out);
// Scans "Item 1.01: ..." mentions into an 8-K item mask.
uint32_t parse_8k_items(std::string_view text);
// Filing index page (…-index.htm): href of the first EX-99* exhibit, or "".
std::string find_exhibit_href(std::string_view index_html);
// Builds a FilingDoc event from an exhibit's HTML, inheriting issuer fields
// from the filing event that triggered the fetch.
void fill_exhibit_event(std::string_view html, const NewsEvent& filing, std::string_view doc_url, NewsEvent& ev,
                        ParseScratch& s);

// ---- halts ----
const char* halt_reason_desc(std::string_view code);

}  // namespace stok
