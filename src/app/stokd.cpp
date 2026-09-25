// stokd: the real-time news + market signal daemon.
//
// Threads (each owns its data; they talk only through lock-free rings):
//   net     FeedPoller: epoll over warm HTTP/TLS connections -> NewsEvent ring
//   market  ITCH / MoldUDP64 / bridge -> seqlocked MarketBoard (+ halt ring)
//   engine  scoring, watchlist, tiers, halts, movers -> Signal + Journal rings
//   alerts  stdout / Telegram (warm connection) / UDP JSON
//   journal JSONL files (news, signals, outcomes)
//   log     async logger

#include <signal.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "app/settings.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/thread.hpp"
#include "engine/engine.hpp"
#include "feeds/parsers.hpp"
#include "feeds/poller.hpp"
#include "market/runner.hpp"
#include "net/http_client.hpp"
#include "ref/filings.hpp"
#include "sink/alerts.hpp"
#include "sink/journal.hpp"
#include "util/file.hpp"
#include "util/time.hpp"

using namespace stok;

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: stokd [-c config/stok.conf] [--set section.key=value ...] [--probe]\n"
               "  --probe   fetch every configured feed once (cold + warm), report latency,\n"
               "            conditional-GET support, item counts and staleness, then exit\n");
}

void setup_thread(const char* name, int cpu, int rt_priority) {
  Logger::set_thread_tag(name);
  set_current_thread_name(std::string("stok-") + name);
  if (cpu >= 0) {
    if (pin_current_thread(cpu)) LOG_INFO("thread %s pinned to cpu %d", name, cpu);
    else LOG_WARN("thread %s: failed to pin to cpu %d", name, cpu);
    if (rt_priority > 0 && !set_realtime_priority(rt_priority))
      LOG_WARN("thread %s: SCHED_FIFO refused (needs CAP_SYS_NICE)", name);
  }
}

// Probe: fetches each feed with the blocking client and reports what matters
// for picking the fastest sources.
int run_probe(const Settings& s, const SymbolTable& symbols) {
  net::TlsContext tls(s.poller.ca_file, s.poller.verify_tls);
  if (!tls.ok()) {
    std::fprintf(stderr, "tls: %s\n", tls.error().c_str());
    return 1;
  }
  net::Resolver resolver;
  net::HttpClient client(tls, resolver, s.poller.user_agent);
  ParseScratch scratch;
  std::printf("%-20s %6s %9s %9s %9s %6s %6s %8s %s\n", "feed", "status", "cold_ms", "warm_ttfb", "bytes", "items",
              "cond", "newest", "sample");
  for (const auto& f : s.feeds) {
    const auto cold = client.get(f.url, 20000, f.extra_headers);
    const auto warm = client.get(f.url, 20000, f.extra_headers);
    std::string cond = "-";
    if (!warm.etag.empty() || !warm.last_modified.empty()) {
      std::string hdr;
      if (!warm.etag.empty()) hdr += "If-None-Match: " + warm.etag + "\r\n";
      if (!warm.last_modified.empty()) hdr += "If-Modified-Since: " + warm.last_modified + "\r\n";
      const auto c = client.get(f.url, 20000, hdr + f.extra_headers);
      cond = c.status == 304 ? "304" : std::to_string(c.status);
    }
    int items = 0;
    int64_t newest = 0;
    std::string sample;
    const std::string& body = warm.body.empty() ? cold.body : warm.body;
    scan_feed(f.kind, body, [&](std::string_view item, uint64_t) {
      ++items;
      NewsEvent ev;
      ev.reset();
      if (!fill_event(f.kind, item, symbols, ev, scratch)) return;
      if (ev.published_ns > newest) newest = ev.published_ns;
      if (sample.empty()) {
        sample = std::string(ev.title.view().substr(0, 60));
        if (ev.n_tickers) sample += " [" + symbols[ev.tickers[0]].ticker + "]";
      }
    });
    char age[32] = "?";
    if (newest > 0) {
      const double secs = static_cast<double>(static_cast<int64_t>(wall_ns()) - newest) / 1e9;
      std::snprintf(age, sizeof(age), "%.0fs", secs);
    }
    std::printf("%-20s %6d %9.1f %9.1f %9zu %6d %6s %8s %s%s\n", f.name.c_str(), cold.status,
                static_cast<double>(cold.total_ns) / 1e6, static_cast<double>(warm.ttfb_ns) / 1e6, body.size(), items,
                cond.c_str(), age, sample.c_str(), cold.error.empty() ? "" : ("  ERROR: " + cold.error).c_str());
  }
  std::printf("\ncold_ms = DNS+TCP+TLS+request; warm_ttfb = request on a kept-alive connection;\n"
              "cond = response to a conditional GET (304 means cheap polling works);\n"
              "newest = age of the newest item (how stale the feed is right now).\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/stok.conf";
  bool probe = false;
  std::vector<std::pair<std::string, std::string>> overrides;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-c" || a == "--config") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--probe") {
      probe = true;
    } else if (a == "--set" && i + 1 < argc) {
      const std::string kv = argv[++i];
      const auto eq = kv.find('=');
      if (eq == std::string::npos) {
        usage();
        return 2;
      }
      overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }

  std::string err;
  auto cfg = Config::load_file(config_path, &err);
  if (!cfg) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  for (const auto& [k, v] : overrides) {
    const auto dot = k.find('.');
    if (dot == std::string::npos) {
      std::fprintf(stderr, "--set expects section.key=value\n");
      return 2;
    }
    cfg->set(k.substr(0, dot), k.substr(dot + 1), v);
  }
  Settings s;
  if (!load_settings(*cfg, s, &err)) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  Logger::instance().start(s.log_file, s.log_level);
  if (s.threads.mlock && !lock_all_memory()) LOG_WARN("mlockall failed (raise RLIMIT_MEMLOCK or grant CAP_IPC_LOCK)");

  // ---- reference data (official sources, downloaded by stok-ref) ----
  SymbolTable symbols;
  std::string report;
  symbols.load_dir(s.ref_dir, &report);
  if (auto b = fileutil::read_file(fileutil::join(s.baseline_dir, "baseline.tsv")))
    report += "baseline=" + std::to_string(symbols.load_baseline(*b));
  LOG_INFO("reference data: %zu symbols (%s)", symbols.size(), report.c_str());
  if (symbols.size() == 0) LOG_WARN("no symbols loaded from %s: run stok-ref first; tickers won't resolve", s.ref_dir.c_str());

  FilingsHistory filings;
  const std::string filings_path = fileutil::join(s.ref_dir, "filings_history.tsv");
  if (auto t = fileutil::read_file(filings_path))
    LOG_INFO("filings history: %zu rows, %zu issuers", filings.load_tsv(*t), filings.issuers());

  Scorer scorer;
  if (!scorer.load_rules_file(s.rules_file, &err)) {
    LOG_ERROR("rules: %s", err.c_str());
    Logger::instance().stop();
    return 1;
  }
  LOG_INFO("scorer: %zu rules, %zu automaton states", scorer.rule_count(), scorer.state_count());

  if (probe) {
    const int rc = run_probe(s, symbols);
    Logger::instance().stop();
    return rc;
  }
  if (s.feeds.empty() && s.market.source == "none") {
    LOG_ERROR("nothing to do: no [feed ...] sections and market.source = none");
    Logger::instance().stop();
    return 1;
  }

  // ---- rings and wakers ----
  SpscQueue<NewsEvent> news_q(s.news_queue);
  SpscQueue<MarketEvent> market_q(s.market_event_queue);
  SpscQueue<Signal> alert_q(s.alert_queue);
  SpscQueue<JournalRecord> journal_q(s.journal_queue);
  Waker engine_waker, alert_waker, journal_waker;

  // ---- components ----
  MarketBoard board(symbols.size());
  const bool have_market = s.market.source != "none";
  MarketRunner market(s.market, symbols, board, market_q, &engine_waker);
  if (!market.init(&err)) {
    LOG_ERROR("market: %s", err.c_str());
    Logger::instance().stop();
    return 1;
  }
  FeedPoller poller(s.poller, s.feeds, s.hosts, symbols, news_q, &engine_waker);
  if (!poller.init(&err)) {
    LOG_ERROR("poller: %s", err.c_str());
    Logger::instance().stop();
    return 1;
  }
  const auto names = feed_names(s);
  Engine engine(s.engine, symbols, have_market ? &board : nullptr, scorer, filings, &alert_q, &alert_waker, &journal_q,
                &journal_waker, names);
  AlertSink alerts(s.alerts, names);
  if (!alerts.init(&err)) {
    LOG_ERROR("alerts: %s", err.c_str());
    Logger::instance().stop();
    return 1;
  }
  Journal journal(s.journal_dir, symbols, &scorer, names);

  // Block termination signals in every thread; main waits for them.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
  signal(SIGPIPE, SIG_IGN);

  std::atomic<bool> stop_producers{false}, stop_engine{false}, stop_sinks{false};
  std::thread t_journal([&] {
    setup_thread("journal", -1, 0);
    journal.run(journal_q, journal_waker, stop_sinks);
  });
  std::thread t_alerts([&] {
    setup_thread("alerts", -1, 0);
    alerts.run(alert_q, alert_waker, stop_sinks);
  });
  std::thread t_engine([&] {
    setup_thread("engine", s.threads.engine_cpu, s.threads.rt_priority);
    engine.run(news_q, market_q, engine_waker, stop_engine);
  });
  std::thread t_market([&] {
    setup_thread("market", s.threads.market_cpu, s.threads.rt_priority);
    market.run(stop_producers);
  });
  std::thread t_net([&] {
    setup_thread("net", s.threads.net_cpu, s.threads.rt_priority);
    poller.run(stop_producers);
  });

  LOG_INFO("stokd running: %zu feeds, market=%s, journal=%s. Ctrl-C to stop.", s.feeds.size(),
           s.market.source.c_str(), s.journal_dir.c_str());
  int sig = 0;
  sigwait(&sigs, &sig);
  LOG_INFO("signal %d: shutting down", sig);

  stop_producers = true;
  t_net.join();
  t_market.join();
  engine_waker.notify();
  stop_engine = true;
  engine_waker.notify();
  t_engine.join();
  stop_sinks = true;
  alert_waker.notify();
  journal_waker.notify();
  t_alerts.join();
  t_journal.join();

  Logger::instance().log_lines(LogLevel::Info, "final feed stats:", poller.stats_report(false));
  Logger::instance().log_lines(LogLevel::Info, "final engine stats:", engine.stats_report(false));
  if (have_market) LOG_INFO("%s", market.stats_report().c_str());
  // Persist filings learned today so tomorrow's dilution flags include them.
  if (fileutil::make_dirs(s.ref_dir) && fileutil::write_file_atomic(filings_path, filings.to_tsv()))
    LOG_INFO("filings history saved (%zu issuers)", filings.issuers());
  Logger::instance().stop();
  return 0;
}
