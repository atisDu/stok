// stok-ref: downloads reference data from official sources only.
//
//   Nasdaq Trader  nasdaqlisted.txt, otherlisted.txt   every US exchange-listed security,
//                                                      listing tier, ETF/test flags,
//                                                      Nasdaq financial-status (deficiency) flags
//   SEC            company_tickers_exchange.json       ticker <-> CIK <-> exchange
//   SEC XBRL       frames: dei:EntityCommonStockSharesOutstanding, dei:EntityPublicFloat
//                                                      -> fundamentals.tsv (shares, float)
//   SEC EDGAR      daily-index master.YYYYMMDD.idx     -> filings_history.tsv (S-1/S-3/424B/
//                                                      EFFECT/PRE 14A/NT... per issuer)
//   FINRA          Reg SHO daily short volume          -> finra_shvol.txt
//
// SEC requires a User-Agent with contact details (net.user_agent) and at most
// 10 requests/second; this tool stays under 8.

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.hpp"
#include "core/ascii.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "net/http_client.hpp"
#include "ref/filings.hpp"
#include "util/file.hpp"
#include "util/json.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

struct Throttle {
  double rps;
  uint64_t last = 0;
  void wait() {
    const uint64_t gap = static_cast<uint64_t>(1e9 / rps);
    const uint64_t now = mono_ns();
    if (last && now - last < gap) std::this_thread::sleep_for(std::chrono::nanoseconds(gap - (now - last)));
    last = mono_ns();
  }
};

struct Ctx {
  net::HttpClient* http;
  Throttle sec{7.5};
  std::string ref_dir;
  int failures = 0;
};

bool fetch_to_file(Ctx& c, const std::string& url, const std::string& name, bool sec_host, std::string* body_out = nullptr) {
  if (sec_host) c.sec.wait();
  auto r = c.http->get(url, 60000);
  if (!r.ok()) {
    std::fprintf(stderr, "  FAIL %s: status %d %s\n", url.c_str(), r.status, r.error.c_str());
    if (r.status == 403 && sec_host)
      std::fprintf(stderr, "  (SEC returns 403 when net.user_agent lacks a name + contact email)\n");
    ++c.failures;
    return false;
  }
  const std::string path = fileutil::join(c.ref_dir, name);
  if (!fileutil::write_file_atomic(path, r.body)) {
    std::fprintf(stderr, "  FAIL writing %s\n", path.c_str());
    ++c.failures;
    return false;
  }
  std::printf("  ok   %-34s %8zu bytes  %6.0f ms\n", name.c_str(), r.body.size(), static_cast<double>(r.total_ns) / 1e6);
  if (body_out) *body_out = std::move(r.body);
  return true;
}

void fetch_symbols(Ctx& c) {
  std::puts("Nasdaq Trader symbol directories:");
  fetch_to_file(c, "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt", "nasdaqlisted.txt", false);
  fetch_to_file(c, "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt", "otherlisted.txt", false);
}

void fetch_sec_tickers(Ctx& c) {
  std::puts("SEC ticker/CIK map:");
  fetch_to_file(c, "https://www.sec.gov/files/company_tickers_exchange.json", "company_tickers_exchange.json", true);
}

// Recent calendar quarters, newest first: {"CY2026Q3I", ...}
std::vector<std::string> recent_quarter_frames(int count) {
  int y;
  unsigned m, d;
  timeutil::eastern_date(static_cast<int64_t>(wall_ns()), y, m, d);
  int q = static_cast<int>((m - 1) / 3) + 1;
  std::vector<std::string> out;
  for (int i = 0; i < count; ++i) {
    out.push_back("CY" + std::to_string(y) + "Q" + std::to_string(q) + "I");
    if (--q == 0) {
      q = 4;
      --y;
    }
  }
  return out;
}

void fetch_fundamentals(Ctx& c, int quarters) {
  std::puts("SEC XBRL frames (shares outstanding, public float):");
  struct Val {
    double v = 0;
    std::string end;
  };
  std::map<uint32_t, Val> shares, flt;
  auto merge = [&](const std::string& concept_path, std::map<uint32_t, Val>& into) {
    for (const auto& frame : recent_quarter_frames(quarters)) {
      c.sec.wait();
      const std::string url = "https://data.sec.gov/api/xbrl/frames/" + concept_path + "/" + frame + ".json";
      auto r = c.http->get(url, 60000);
      if (r.status == 404) continue;  // frame not published yet
      if (!r.ok()) {
        std::fprintf(stderr, "  FAIL %s: %d %s\n", url.c_str(), r.status, r.error.c_str());
        ++c.failures;
        continue;
      }
      JsonDoc doc;
      if (!doc.parse(r.body)) {
        std::fprintf(stderr, "  FAIL parsing %s: %s\n", url.c_str(), doc.error().c_str());
        continue;
      }
      const auto* data = doc.get(doc.root(), "data");
      if (!data) continue;
      std::size_t n = 0;
      doc.for_each(*data, [&](const JsonDoc::Node& row) {
        const uint32_t cik = static_cast<uint32_t>(JsonDoc::num(doc.get(row, "cik")));
        const double v = JsonDoc::num(doc.get(row, "val"));
        const std::string end = JsonDoc::str(doc.get(row, "end"));
        if (!cik || v <= 0) return;
        Val& cur = into[cik];
        if (end > cur.end) cur = Val{v, end};
        ++n;
      });
      std::printf("  ok   %-48s %6zu rows\n", (concept_path + "/" + frame).c_str(), n);
    }
  };
  merge("dei/EntityCommonStockSharesOutstanding/shares", shares);
  merge("dei/EntityPublicFloat/USD", flt);
  std::set<uint32_t> ciks;
  for (auto& [k, v] : shares) ciks.insert(k);
  for (auto& [k, v] : flt) ciks.insert(k);
  std::string out = "cik\tshares_outstanding\tshares_asof\tpublic_float_usd\tfloat_asof\n";
  char line[160];
  for (uint32_t cik : ciks) {
    const Val s = shares.count(cik) ? shares[cik] : Val{};
    const Val f = flt.count(cik) ? flt[cik] : Val{};
    std::snprintf(line, sizeof(line), "%u\t%.0f\t%s\t%.0f\t%s\n", cik, s.v, s.end.c_str(), f.v, f.end.c_str());
    out += line;
  }
  fileutil::write_file_atomic(fileutil::join(c.ref_dir, "fundamentals.tsv"), out);
  std::printf("  wrote fundamentals.tsv (%zu issuers)\n", ciks.size());
}

void fetch_finra(Ctx& c) {
  std::puts("FINRA Reg SHO daily short volume:");
  const int64_t now = static_cast<int64_t>(wall_ns());
  for (int back = 1; back <= 7; ++back) {
    int y;
    unsigned m, d;
    timeutil::eastern_date(now - back * 86400LL * 1'000'000'000LL, y, m, d);
    const int64_t days = timeutil::days_from_civil(y, m, d);
    const int wd = timeutil::weekday_from_days(days);
    if (wd == 0 || wd == 6) continue;
    char date[16];
    std::snprintf(date, sizeof(date), "%04d%02u%02u", y, m, d);
    auto r = c.http->get(std::string("https://cdn.finra.org/equity/regsho/daily/CNMSshvol") + date + ".txt", 30000);
    if (r.ok() && !r.body.empty()) {
      fileutil::write_file_atomic(fileutil::join(c.ref_dir, "finra_shvol.txt"), r.body);
      std::printf("  ok   CNMSshvol%s.txt %zu bytes\n", date, r.body.size());
      return;
    }
  }
  std::fprintf(stderr, "  FAIL: no FINRA short-volume file in the last 7 days\n");
  ++c.failures;
}

void fetch_filings(Ctx& c, int days) {
  std::printf("EDGAR daily form indexes (last %d days):\n", days);
  FilingsHistory hist;
  const std::string hist_path = fileutil::join(c.ref_dir, "filings_history.tsv");
  const std::string done_path = fileutil::join(c.ref_dir, "filings_days.txt");
  if (auto t = fileutil::read_file(hist_path)) hist.load_tsv(*t);
  std::set<std::string> done;
  if (auto t = fileutil::read_file(done_path)) for_each_line(*t, [&](std::string_view l) {
      if (!l.empty()) done.insert(std::string(l));
    });
  const int64_t now = static_cast<int64_t>(wall_ns());
  int fetched = 0, skipped = 0, missing = 0;
  std::size_t rows = 0;
  for (int back = 1; back <= days; ++back) {
    int y;
    unsigned m, d;
    timeutil::eastern_date(now - back * 86400LL * 1'000'000'000LL, y, m, d);
    const int wd = timeutil::weekday_from_days(timeutil::days_from_civil(y, m, d));
    if (wd == 0 || wd == 6) continue;
    char ymd[16];
    std::snprintf(ymd, sizeof(ymd), "%04d%02u%02u", y, m, d);
    if (done.count(ymd)) {
      ++skipped;
      continue;
    }
    c.sec.wait();
    char url[160];
    std::snprintf(url, sizeof(url), "https://www.sec.gov/Archives/edgar/daily-index/%04d/QTR%u/master.%s.idx", y,
                  (m - 1) / 3 + 1, ymd);
    auto r = c.http->get(url, 60000);
    if (r.status == 404 || r.status == 403) {
      if (r.status == 403) {
        std::fprintf(stderr, "  SEC returned 403: check net.user_agent (needs name + email). Stopping.\n");
        ++c.failures;
        break;
      }
      ++missing;  // market holiday
      done.insert(ymd);
      continue;
    }
    if (!r.ok()) {
      std::fprintf(stderr, "  FAIL %s: %d %s\n", url, r.status, r.error.c_str());
      ++c.failures;
      continue;
    }
    rows += hist.add_master_index(r.body);
    done.insert(ymd);
    if (++fetched % 20 == 0) std::printf("  ... %d days fetched\n", fetched);
  }
  fileutil::write_file_atomic(hist_path, hist.to_tsv());
  std::string done_txt;
  for (const auto& dd : done) done_txt += dd + "\n";
  fileutil::write_file_atomic(done_path, done_txt);
  std::printf("  fetched %d day(s), %d already had, %d holidays; +%zu relevant filings, %zu issuers total\n", fetched,
              skipped, missing, rows, hist.issuers());
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep progress and errors in order
  std::string config_path = "config/stok.conf";
  std::set<std::string> only;
  int days = 365;
  int quarters = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
    else if (a == "--days" && i + 1 < argc) days = std::atoi(argv[++i]);
    else if (a == "--quarters" && i + 1 < argc) quarters = std::atoi(argv[++i]);
    else if (a == "--only" && i + 1 < argc) {
      split(argv[++i], ',', [&](std::string_view p) { only.insert(std::string(trim(p))); });
    } else {
      std::fprintf(stderr,
                   "usage: stok-ref [-c config] [--only symbols,sec,facts,finra,filings] [--days 365] [--quarters 5]\n");
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  std::string err;
  auto cfg = Config::load_file(config_path, &err);
  if (!cfg) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  Settings s;
  if (!load_settings(*cfg, s, &err)) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  if (s.poller.user_agent.find('@') == std::string::npos)
    std::fprintf(stderr, "warning: net.user_agent has no contact email; SEC will likely refuse requests\n");
  fileutil::make_dirs(s.ref_dir);
  net::TlsContext tls(s.poller.ca_file, s.poller.verify_tls);
  if (!tls.ok()) {
    std::fprintf(stderr, "tls: %s\n", tls.error().c_str());
    return 1;
  }
  net::Resolver resolver;
  net::HttpClient http(tls, resolver, s.poller.user_agent);
  Ctx c{&http, Throttle{7.5}, s.ref_dir, 0};
  auto want = [&](const char* k) { return only.empty() || only.count(k); };
  if (want("symbols")) fetch_symbols(c);
  if (want("sec")) fetch_sec_tickers(c);
  if (want("facts")) fetch_fundamentals(c, quarters);
  if (want("finra")) fetch_finra(c);
  if (want("filings")) fetch_filings(c, days);
  Logger::instance().stop();
  std::printf("%s (%d failure%s). Files in %s\n", c.failures ? "done with errors" : "done", c.failures,
              c.failures == 1 ? "" : "s", s.ref_dir.c_str());
  return c.failures ? 1 : 0;
}
