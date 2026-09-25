// Integration driver: runs the real FeedPoller (epoll + TLS + keep-alive +
// gzip + chunked + ETag) against the local HTTPS server started by
// run_http_test.py, and checks what reaches the engine ring.
// Usage: stok-it-http <port> <ca_cert.pem> <fixtures_dir>

#include <cstdio>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "feeds/poller.hpp"
#include "net/http_client.hpp"
#include "util/file.hpp"

using namespace stok;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: stok-it-http <port> <ca.pem> <fixtures_dir>\n");
    return 2;
  }
  const std::string port = argv[1], ca = argv[2], fix = argv[3];
  Logger::instance().start("", LogLevel::Warn);
  SymbolTable syms;
  syms.load_nasdaq_listed(fileutil::read_file(fix + "/nasdaqlisted.txt").value_or(""));
  syms.load_other_listed(fileutil::read_file(fix + "/otherlisted.txt").value_or(""));
  syms.load_sec_tickers(fileutil::read_file(fix + "/company_tickers_exchange.json").value_or(""));

  const std::string base = "https://localhost:" + port;
  PollerOptions po;
  po.user_agent = "stok-it test@example.com";
  po.ca_file = ca;
  po.fetch_exhibits = true;
  po.stats_interval_s = 0;
  std::vector<FeedSpec> feeds = {
      {"wire", base + "/feed.rss", FeedKind::Rss, 100, 2, false, ""},
      {"wire_chunked", base + "/chunked.rss", FeedKind::Rss, 150, 1, false, ""},
      {"edgar", base + "/edgar.atom", FeedKind::EdgarAtom, 100, 2, false, ""},
  };
  std::vector<HostPolicy> hosts = {{"localhost", 100.0}};
  SpscQueue<NewsEvent> q(256);
  FeedPoller poller(po, feeds, hosts, syms, q, nullptr);
  std::string err;
  if (!poller.init(&err)) {
    std::fprintf(stderr, "init: %s\n", err.c_str());
    return 1;
  }
  int news_acmr = 0, news_dltx = 0, filings = 0, docs = 0;
  uint64_t first_news_latency_us = 0;
  const uint64_t t_end = mono_ns() + 4 * kNsPerSec;
  while (mono_ns() < t_end) {
    poller.step(5 * kNsPerMs);
    while (NewsEvent* ev = q.front()) {
      const std::string t = ev->n_tickers ? syms[ev->tickers[0]].ticker : "-";
      std::printf("event src=%s kind=%s ticker=%s title=\"%.70s\"\n", poller.spec(ev->source).name.c_str(),
                  event_kind_name(ev->kind), t.c_str(), ev->title.c_str());
      if (ev->kind == EventKind::News && t == "ACMR") {
        ++news_acmr;
        if (!first_news_latency_us) first_news_latency_us = (ev->parsed_ns - ev->recv_ns) / 1000;
      }
      if (ev->kind == EventKind::News && t == "DLTX") ++news_dltx;
      if (ev->kind == EventKind::Filing) ++filings;
      if (ev->kind == EventKind::FilingDoc) ++docs;
      q.pop();
    }
  }
  std::printf("%s", poller.stats_report(false).c_str());

  // Blocking client: warm-connection POST (what alerts use).
  net::TlsContext tls(ca, true);
  net::Resolver res;
  net::HttpClient http(tls, res, "stok-it");
  auto r1 = http.post(base + "/post", "{\"x\":1}", "application/json");
  auto r2 = http.post(base + "/post", "{\"x\":22}", "application/json");
  std::printf("post: %d %s | cold %.2f ms, warm %.2f ms (reused=%d)\n", r1.status, r2.body.c_str(),
              static_cast<double>(r1.total_ns) / 1e6, static_cast<double>(r2.total_ns) / 1e6, r2.reused ? 1 : 0);

  bool ok = true;
  auto expect = [&](bool c, const char* what) {
    if (!c) {
      std::printf("FAIL: %s\n", what);
      ok = false;
    }
  };
  expect(news_acmr == 2, "the new ACMR story arrives once per wire feed (2 feeds)");
  expect(news_dltx == 0, "stories present at start-up are not emitted (primed)");
  expect(filings == 1, "one new 8-K filing emitted");
  expect(docs == 1, "the 8-K's EX-99.1 exhibit fetched and emitted");
  expect(first_news_latency_us < 5000, "parse-to-ring latency under 5 ms");
  expect(r1.status == 200 && r2.status == 200 && r2.reused, "HttpClient reuses its keep-alive connection");
  expect(poller.stats(0).not_modified > 0, "wire feed saw 304 Not Modified");
  expect(poller.stats(0).connects <= 4, "wire feed lanes stayed on warm connections");
  Logger::instance().stop();
  std::printf("it_http: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
