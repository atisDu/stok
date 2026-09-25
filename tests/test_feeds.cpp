#include <vector>

#include "check.hpp"
#include "feeds/parsers.hpp"
#include "feeds/tickers.hpp"
#include "feeds/xml.hpp"
#include "fixture_symbols.hpp"
#include "ref/filings.hpp"

using namespace stok;

namespace {

std::vector<NewsEvent> parse_all(FeedKind kind, const std::string& doc, const SymbolTable& syms) {
  std::vector<NewsEvent> out;
  ParseScratch s;
  scan_feed(kind, doc, [&](std::string_view item, uint64_t id) {
    NewsEvent ev;
    ev.reset();
    if (fill_event(kind, item, syms, ev, s)) {
      ev.id_hash = id;
      out.push_back(ev);
    }
  });
  return out;
}

std::string ticker_of(const SymbolTable& syms, const NewsEvent& e, int i = 0) {
  return i < e.n_tickers ? syms[e.tickers[i]].ticker : std::string("-");
}

}  // namespace

TEST(xml_scanner_basics) {
  const std::string doc =
      "<rss><item><title><![CDATA[A </item> inside CDATA]]></title><link>x</link></item>"
      "<item><title>B &amp; C</title><linkx>no</linkx><link href=\"h\"/></item></rss>";
  std::vector<std::string> titles;
  xml::for_each_item(doc, "item", [&](std::string_view item, std::string_view) {
    titles.emplace_back(xml::child_text(item, "title"));
  });
  CHECK_EQ(titles.size(), 2u);
  if (titles.size() == 2) {
    CHECK_EQ(titles[0], std::string("<![CDATA[A </item> inside CDATA]]>"));
    CHECK_EQ(titles[1], std::string("B &amp; C"));
  }
  CHECK_EQ(std::string(xml::unwrap_cdata("<![CDATA[x<y]]>")), std::string("x<y"));
  CHECK_EQ(std::string(xml::attr_value(" rel=\"alternate\" href='u?a=1&amp;b=2'", "href")), std::string("u?a=1&amp;b=2"));
}

TEST(ticker_extraction_variants) {
  const SymbolTable syms = test::fixture_symbols();
  TickerHit hits[16];
  auto run = [&](const char* text) {
    std::vector<std::string> out;
    const int n = extract_tickers(text, syms, hits, 16);
    for (int i = 0; i < n; ++i) out.push_back(hits[i].raw);
    return out;
  };
  CHECK_EQ(run("Acme Robotics, Inc. (NASDAQ: ACMR) today").size(), 1u);
  CHECK_EQ(run("(Nasdaq:ACMR, ACMRW)").size(), 2u);
  const auto multi = run("Echo Mining Corp. (TSXV: ECM) (OTCQB: ECMGF) announced");
  CHECK_EQ(multi.size(), 2u);
  if (multi.size() == 2) {
    CHECK_EQ(multi[0], std::string("TSXV:ECM"));
    CHECK_EQ(multi[1], std::string("OTC:ECMGF"));
  }
  CHECK_EQ(run("Foxtrot Mining (NYSE American: FOXT)")[0], std::string("FOXT"));
  CHECK_EQ(run("Berkshire (NYSE: BRK.B).")[0], std::string("BRK.B"));
  CHECK(run("Listed on Nasdaq: The company said").empty());   // prose, not a ticker
  CHECK(run("ratio 3:1 and time 10:30").empty());
  // Resolution: OTC symbol known via SEC map, US symbol via the directory.
  const int n = extract_tickers("(OTCQB: ECMGF) (NASDAQ: DLTX)", syms, hits, 16);
  CHECK_EQ(n, 2);
  CHECK(hits[0].sym != SymbolTable::kInvalid && hits[0].venue == VenueClass::OTC);
  CHECK(hits[1].sym == syms.find("DLTX"));
}

TEST(parse_globenewswire_rss) {
  const SymbolTable syms = test::fixture_symbols();
  const auto evs = parse_all(FeedKind::Rss, test::fixture("globenewswire.xml"), syms);
  CHECK_EQ(evs.size(), 3u);
  if (evs.size() < 3) return;
  const NewsEvent& a = evs[0];
  CHECK(a.kind == EventKind::News);
  CHECK_EQ(std::string(a.title.view()), std::string("Acme Robotics Signs $20 Million Supply Agreement with Walmart"));
  CHECK_EQ(ticker_of(syms, a), std::string("ACMR"));
  CHECK_EQ(a.n_tickers, 1);  // category + body both name ACMR: deduplicated
  CHECK(a.published_ns == 1790337600LL * 1'000'000'000LL);
  CHECK(a.body.view().find("(\"Acme\") (NASDAQ: ACMR)") != std::string_view::npos);  // double-encoded entities decoded
  CHECK(a.link.view().find("3155123") != std::string_view::npos);
  CHECK_EQ(ticker_of(syms, evs[1]), std::string("BRVO"));
  CHECK_EQ(ticker_of(syms, evs[2]), std::string("CHLY"));
  CHECK(evs[0].id_hash != evs[1].id_hash);
}

TEST(parse_prnewswire_rss_cdata_and_foreign_tickers) {
  const SymbolTable syms = test::fixture_symbols();
  const auto evs = parse_all(FeedKind::Rss, test::fixture("prnewswire.xml"), syms);
  CHECK_EQ(evs.size(), 2u);
  if (evs.size() < 2) return;
  CHECK_EQ(ticker_of(syms, evs[0]), std::string("DLTX"));
  CHECK(evs[0].body.view().find("\xE2\x80\x94") != std::string_view::npos);  // &#8212; decoded inside CDATA
  CHECK_EQ(evs[0].published_ns, 1790337600LL * 1'000'000'000LL);
  // Echo: OTC ticker resolves through the SEC map; TSXV kept as unresolved.
  CHECK_EQ(ticker_of(syms, evs[1]), std::string("ECMGF"));
  CHECK_EQ(evs[1].n_unresolved, 1);
  CHECK_EQ(std::string(evs[1].unresolved[0]), std::string("TSXV:ECM"));
}

TEST(parse_edgar_atom) {
  const SymbolTable syms = test::fixture_symbols();
  const auto evs = parse_all(FeedKind::EdgarAtom, test::fixture("edgar_current.xml"), syms);
  CHECK_EQ(evs.size(), 3u);
  if (evs.size() < 3) return;
  const NewsEvent& a = evs[0];
  CHECK(a.kind == EventKind::Filing);
  CHECK_EQ(std::string(a.form), std::string("8-K"));
  CHECK_EQ(a.cik, 1234567u);
  CHECK_EQ(form8k_items_str(a.items_mask), std::string("1.01,7.01,9.01"));
  CHECK_EQ(ticker_of(syms, a), std::string("ACMR"));        // primary security first
  CHECK_EQ(ticker_of(syms, a, 1), std::string("ACMRW"));
  CHECK(a.title.view().find("Entry into a Material Definitive Agreement") != std::string_view::npos);
  CHECK(a.link.view().find("-index.htm") != std::string_view::npos);
  CHECK_EQ(a.published_ns, (1790337600LL + 47) * 1'000'000'000LL);  // 08:00:47 EDT = 12:00:47 UTC
  CHECK_EQ(form8k_items_str(evs[1].items_mask), std::string("3.02,9.01"));
  CHECK_EQ(std::string(evs[2].form), std::string("424B5"));
  EdgarTitle t;
  CHECK(parse_edgar_title("SC 13D - SOME CO (FUND) LP (0000123456) (Subject)", t));
  CHECK_EQ(std::string(t.form), std::string("SC 13D"));
  CHECK_EQ(t.cik, 123456u);
  CHECK_EQ(std::string(t.company), std::string("SOME CO (FUND) LP"));
  CHECK_EQ(std::string(t.role), std::string("Subject"));
}

TEST(parse_nasdaq_halts) {
  const SymbolTable syms = test::fixture_symbols();
  const auto evs = parse_all(FeedKind::NasdaqHalts, test::fixture("halts.xml"), syms);
  CHECK_EQ(evs.size(), 2u);
  if (evs.size() < 2) return;
  CHECK(evs[0].kind == EventKind::Halt);
  CHECK_EQ(ticker_of(syms, evs[0]), std::string("ACMR"));
  CHECK_EQ(std::string(evs[0].halt.reason), std::string("LUDP"));
  CHECK_NEAR(evs[0].halt.pause_threshold, 3.15, 1e-9);
  CHECK_EQ(evs[0].halt.resume_trade_ns, 0);
  CHECK(evs[0].title.view().find("HALT ACMR") == 0);
  CHECK(evs[1].halt.resume_trade_ns > evs[1].halt.halt_ns);
  CHECK(evs[1].title.view().find("RESUME DLTX") == 0);
  // The id covers the mutable resumption fields: an update is a new event.
  std::string doc = test::fixture("halts.xml");
  uint64_t before = 0, after = 0;
  scan_feed(FeedKind::NasdaqHalts, doc, [&](std::string_view, uint64_t id) { if (!before) before = id; });
  const auto p = doc.find("<ndaq:ResumptionTradeTime></ndaq:ResumptionTradeTime>");
  doc.replace(p, 53, "<ndaq:ResumptionTradeTime>10:10:12</ndaq:ResumptionTradeTime>");
  scan_feed(FeedKind::NasdaqHalts, doc, [&](std::string_view, uint64_t id) { if (!after) after = id; });
  CHECK(before != after);
}

TEST(edgar_exhibit_discovery_and_event) {
  const SymbolTable syms = test::fixture_symbols();
  const std::string href = find_exhibit_href(test::fixture("edgar_index.htm"));
  CHECK_EQ(href, std::string("/Archives/edgar/data/1234567/000123456726000012/ex99-1.htm"));
  const auto filings = parse_all(FeedKind::EdgarAtom, test::fixture("edgar_current.xml"), syms);
  if (filings.empty()) return;
  ParseScratch s;
  NewsEvent ev;
  fill_exhibit_event(test::fixture("ex99-1.htm"), filings[0], "https://www.sec.gov" + href, ev, s);
  CHECK(ev.kind == EventKind::FilingDoc);
  CHECK_EQ(std::string(ev.title.view()), std::string("Acme Robotics Signs $20 Million Supply Agreement with Walmart"));
  CHECK_EQ(ev.cik, 1234567u);
  CHECK(ev.body.view().find("definitive three-year supply agreement") != std::string_view::npos);
  // Same headline as the wire story -> same key -> cross-source "who was first".
  const auto wires = parse_all(FeedKind::Rss, test::fixture("globenewswire.xml"), syms);
  if (!wires.empty()) CHECK_EQ(ev.title_key, wires[0].title_key);
}

TEST(company_name_fallback_for_untagged_stories) {
  const SymbolTable syms = test::fixture_symbols();
  const std::string doc =
      "<rss><channel>"
      "<item><guid>a</guid><title>Acme Robotics Launches Next-Generation Warehouse Robot</title>"
      "<description>NEW YORK, Sept. 25, 2026 (GLOBE NEWSWIRE) -- Acme Robotics, Inc. today announced a new robot."
      "</description></item>"
      "<item><guid>b</guid><title>Quarterly Update</title><dc:contributor>Delta Therapeutics, Inc.</dc:contributor>"
      "<description>Update for shareholders.</description></item>"
      "<item><guid>c</guid><title>Delta Therapeutics&#8217;s DLX-101 Data Presented at Congress</title>"
      "<description>Data were presented.</description></item>"
      "<item><guid>d</guid><title>Industry Group Publishes Robotics Outlook</title>"
      "<description>A trade group published a report.</description></item>"
      "</channel></rss>";
  const auto evs = parse_all(FeedKind::Rss, doc, syms);
  CHECK_EQ(evs.size(), 4u);
  if (evs.size() < 4) return;
  CHECK_EQ(ticker_of(syms, evs[0]), std::string("ACMR"));
  CHECK(evs[0].flags & kEvNameMatched);
  CHECK_EQ(ticker_of(syms, evs[1]), std::string("DLTX"));  // dc:contributor
  CHECK_EQ(ticker_of(syms, evs[2]), std::string("DLTX"));  // headline subject with possessive
  CHECK_EQ(evs[3].n_tickers, 0);                           // no false positive
  // Tagged stories don't use (or get flagged by) the fallback.
  const auto gnw = parse_all(FeedKind::Rss, test::fixture("globenewswire.xml"), syms);
  if (!gnw.empty()) CHECK(!(gnw[0].flags & kEvNameMatched));
}
