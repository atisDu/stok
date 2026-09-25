// stok-history: builds journal-format news for past days from SEC EDGAR's
// official archives (8-K/6-K filings with their EX-99 press releases, plus
// dilution filings), stamped at acceptance time. Feed the result to
// stok-backtest --news.
//
//   stok-history -c config/stok.conf --from 2019-01-28 --to 2019-02-01
//   stok-backtest -c config/stok.conf --news data/journal-edgar --itch data/itch

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.hpp"
#include "core/ascii.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "net/http_client.hpp"
#include "research/edgar_history.hpp"
#include "sink/journal.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

// SEC-friendly fetcher: throttled below 10 req/s, with an on-disk cache so
// re-runs cost nothing.
class CachedSecFetcher {
 public:
  CachedSecFetcher(net::HttpClient& http, std::string cache_dir) : http_(http), cache_(std::move(cache_dir)) {}

  std::optional<std::string> get(const std::string& url) {
    std::string key = url;
    const auto p = key.find("://");
    if (p != std::string::npos) key = key.substr(p + 3);
    for (auto& c : key)
      if (c == '?' || c == '&' || c == '=' || c == ':') c = '_';
    const std::string path = fileutil::join(cache_, key);
    if (auto cached = fileutil::read_file(path)) return cached;
    if (fileutil::exists(path + ".404")) return std::nullopt;
    const uint64_t gap = static_cast<uint64_t>(1e9 / 7.5);
    const uint64_t now = mono_ns();
    if (last_ && now - last_ < gap) std::this_thread::sleep_for(std::chrono::nanoseconds(gap - (now - last_)));
    last_ = mono_ns();
    ++requests_;
    auto r = http_.get(url, 60000);
    const auto slash = path.rfind('/');
    fileutil::make_dirs(path.substr(0, slash));
    if (r.status == 404) {
      fileutil::write_file_atomic(path + ".404", "");
      return std::nullopt;
    }
    if (!r.ok()) {
      ++errors_;
      if (r.status == 403) std::fprintf(stderr, "SEC returned 403 for %s: set net.user_agent to 'Name email'\n", url.c_str());
      return std::nullopt;
    }
    fileutil::write_file_atomic(path, r.body);
    return r.body;
  }
  uint64_t requests() const { return requests_; }

 private:
  net::HttpClient& http_;
  std::string cache_;
  uint64_t last_ = 0, requests_ = 0, errors_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string config_path = "config/stok.conf", out_dir, cache_dir, from, to;
  std::vector<std::string> dates;
  research::EdgarHistoryOptions o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "-c" || a == "--config") config_path = next();
    else if (a == "--date") dates.push_back(next());
    else if (a == "--from") from = next();
    else if (a == "--to") to = next();
    else if (a == "--out") out_dir = next();
    else if (a == "--cache") cache_dir = next();
    else if (a == "--all-issuers") o.listed_only = false;
    else if (a == "--no-exhibits") o.fetch_exhibits = false;
    else if (a == "--no-dilution") o.dilution_forms = false;
    else if (a == "--delay-ms") o.dissemination_delay_ms = std::atoll(next().c_str());
    else if (a == "--max") o.max_filings = static_cast<std::size_t>(std::atoll(next().c_str()));
    else {
      std::fprintf(stderr,
                   "usage: stok-history [-c config] (--date YYYY-MM-DD ... | --from D --to D) [--out DIR] [--cache DIR]\n"
                   "                    [--all-issuers] [--no-exhibits] [--no-dilution] [--delay-ms 30000] [--max N]\n");
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  std::string err;
  auto cfg = Config::load_file(config_path, &err);
  Settings s;
  if (!cfg || !load_settings(*cfg, s, &err)) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  if (out_dir.empty()) out_dir = fileutil::join(s.data_dir, "journal-edgar");
  if (cache_dir.empty()) cache_dir = fileutil::join(s.data_dir, "cache/edgar");
  if (!from.empty()) {
    int y0, y1;
    unsigned m0, d0, m1, d1;
    if (!timeutil::parse_ymd(from, y0, m0, d0) || !timeutil::parse_ymd(to.empty() ? from : to, y1, m1, d1)) {
      std::fprintf(stderr, "bad --from/--to\n");
      return 2;
    }
    for (int64_t day = timeutil::days_from_civil(y0, m0, d0); day <= timeutil::days_from_civil(y1, m1, d1); ++day) {
      const int wd = timeutil::weekday_from_days(day);
      if (wd == 0 || wd == 6) continue;
      int y;
      unsigned m, d;
      timeutil::civil_from_days(day, y, m, d);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
      dates.push_back(buf);
    }
  }
  if (dates.empty()) {
    std::fprintf(stderr, "no dates given\n");
    return 2;
  }
  SymbolTable symbols;
  std::string rep;
  symbols.load_dir(s.ref_dir, &rep);
  if (symbols.size() == 0) std::fprintf(stderr, "warning: no reference data in %s (run stok-ref)\n", s.ref_dir.c_str());

  net::TlsContext tls(s.poller.ca_file, s.poller.verify_tls);
  net::Resolver resolver;
  net::HttpClient http(tls, resolver, s.poller.user_agent);
  CachedSecFetcher fetcher(http, cache_dir);
  const std::vector<std::string> sources = {"edgar_history", "edgar_history_ex99"};
  for (const auto& date : dates) {
    std::vector<NewsEvent> events;
    const auto st = research::build_edgar_day(date, symbols, o,
                                              [&](const std::string& url) { return fetcher.get(url); }, events);
    const std::string day_dir = fileutil::join(out_dir, date);
    fileutil::make_dirs(day_dir);
    Journal j(out_dir, symbols, nullptr, sources);
    std::string jsonl;
    for (const auto& e : events) {
      JournalRecord r{};
      r.type = JournalRecord::Type::News;
      r.news = e;
      r.news_sym = e.n_tickers ? e.tickers[0] : SymbolTable::kInvalid;
      r.engine_ns = e.recv_ns;
      jsonl += j.news_json(r) + "\n";
    }
    fileutil::write_file_atomic(fileutil::join(day_dir, "news.jsonl"), jsonl);
    std::printf("%s: index rows=%zu candidates=%zu filings=%zu exhibits=%zu untimed=%zu errors=%zu -> %s\n",
                date.c_str(), st.index_rows, st.candidates, st.filings, st.exhibits, st.missing_accepted, st.errors,
                fileutil::join(day_dir, "news.jsonl").c_str());
  }
  std::printf("SEC requests: %llu (cached pages are reused on re-runs)\n",
              static_cast<unsigned long long>(fetcher.requests()));
  Logger::instance().stop();
  return 0;
}
