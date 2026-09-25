#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/flat_hash.hpp"
#include "core/histogram.hpp"
#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/events.hpp"
#include "feeds/parsers.hpp"
#include "net/buffer.hpp"
#include "net/connection.hpp"
#include "net/resolver.hpp"
#include "net/tls.hpp"
#include "net/url.hpp"
#include "ref/symbols.hpp"

namespace stok {

struct FeedSpec {
  std::string name;
  std::string url;
  FeedKind kind = FeedKind::Rss;
  uint32_t interval_ms = 1000;  // effective poll period for the feed
  uint32_t lanes = 1;           // warm connections polling in staggered phases
  bool emit_backlog = false;    // emit items already present at start-up
  std::string extra_headers;    // preformatted "Name: value\r\n"
};

struct HostPolicy {
  std::string host;
  double max_rps = 5.0;  // token-bucket rate shared by every request to the host
};

struct PollerOptions {
  std::string user_agent = "stok/0.1 (set net.user_agent to 'Company contact@example.com')";
  std::string ca_file;
  bool verify_tls = true;
  bool busy_poll = false;          // spin on epoll instead of sleeping (burns a core)
  bool fetch_exhibits = true;      // fetch EX-99 press releases attached to 8-K/6-K filings
  double exhibit_max_shares = 500e6;
  std::size_t exhibit_queue_max = 256;
  int connect_timeout_ms = 3000;
  int io_timeout_ms = 8000;
  double default_max_rps = 5.0;
  int stats_interval_s = 60;
};

// The news path's network front end. One thread, one epoll set, many
// persistent connections:
//
// * Each feed has N "lanes" (warm keep-alive connections) that fire in
//   staggered phases, so the feed is polled every interval_ms even when a
//   single response is slow, and no poll ever waits for a TCP/TLS handshake.
// * Conditional GETs (If-None-Match / If-Modified-Since): unchanged feeds
//   cost a ~200-byte 304 instead of the whole document.
// * Per-host token buckets enforce each publisher's fair-access limits (SEC:
//   10 req/s) across all feeds and exhibit fetches.
// * Two-pass parsing: pass 1 hashes each item's id; only unseen items are
//   parsed into events, written in place into the engine's lock-free ring.
// * Idle connections the server closes are reopened immediately, and
//   connections nearing the server's Keep-Alive timeout are refreshed early.
class FeedPoller {
 public:
  struct FeedStats {
    uint64_t requests = 0, ok = 0, not_modified = 0, http_errors = 0, net_errors = 0, connects = 0;
    uint64_t new_items = 0, dropped = 0, skipped_busy = 0, rate_limited = 0, bytes = 0;
    LatencyHistogram ttfb, total, parse, connect;
  };
  struct ExhibitStats {
    uint64_t queued = 0, fetched = 0, none = 0, errors = 0, overflow = 0;
    LatencyHistogram latency;  // filing seen -> exhibit event published
  };

  FeedPoller(PollerOptions opts, std::vector<FeedSpec> feeds, std::vector<HostPolicy> hosts,
             const SymbolTable& symbols, SpscQueue<NewsEvent>& out, Waker* out_waker);
  ~FeedPoller();

  // Resolves hosts, creates lanes and opens connections.
  bool init(std::string* err);
  void run(const std::atomic<bool>& stop);
  // One loop iteration: fire due polls, wait for I/O up to max_wait_ns, handle it.
  void step(uint64_t max_wait_ns);

  std::size_t feed_count() const { return feeds_.size(); }
  const FeedSpec& spec(std::size_t i) const;
  const FeedStats& stats(std::size_t i) const;
  const ExhibitStats& exhibit_stats() const { return exhibit_st_; }
  std::string stats_report(bool reset);

 private:
  struct Host;
  struct Feed;
  struct Lane;
  struct JobLane;
  struct IoTag {
    uint8_t type;  // 0 = lane, 1 = job lane
    uint32_t index;
  };

  void service_lane(Lane& lane, uint64_t now);
  void service_jobs(JobLane& jl, uint64_t now);
  void start_connect(net::Connection& c, Host& h, uint64_t now, uint64_t& reconnect_at, uint32_t& fail_streak);
  void on_lane_event(Lane& lane, net::Connection::Event ev, uint64_t now);
  void on_job_event(JobLane& jl, net::Connection::Event ev, uint64_t now);
  void on_response(Lane& lane, uint64_t now);
  void on_job_response(JobLane& jl, uint64_t now);
  void maybe_queue_exhibit(const NewsEvent& ev);
  void check_timeouts(uint64_t now);
  uint64_t next_deadline(uint64_t now) const;

  PollerOptions opts_;
  std::vector<FeedSpec> specs_;
  std::vector<HostPolicy> host_policies_;
  const SymbolTable& symbols_;
  SpscQueue<NewsEvent>& out_;
  Waker* out_waker_;

  std::unique_ptr<net::TlsContext> tls_;
  net::Resolver resolver_;
  int ep_ = -1;
  std::vector<std::unique_ptr<Host>> hosts_;
  std::vector<std::unique_ptr<Feed>> feeds_;
  std::vector<std::unique_ptr<Lane>> lanes_;
  std::vector<std::unique_ptr<JobLane>> job_lanes_;
  ParseScratch scratch_;
  ExhibitStats exhibit_st_;
  uint64_t last_timeout_check_ = 0;
  uint64_t last_stats_ = 0;
};

}  // namespace stok
