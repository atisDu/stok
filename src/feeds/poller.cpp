#include "feeds/poller.hpp"

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "ref/filings.hpp"

namespace stok {

struct FeedPoller::Host {
  std::string name;
  uint16_t port = 443;
  bool tls = true;
  net::Address addr;
  double max_rps = 5.0;
  double tokens = 1.0;
  double burst = 1.0;
  uint64_t last_refill = 0;

  void refill(uint64_t now) {
    if (last_refill == 0) last_refill = now;
    const double dt = static_cast<double>(now - last_refill) / 1e9;
    last_refill = now;
    tokens = std::min(burst, tokens + dt * max_rps);
  }
  bool take(uint64_t now) {
    refill(now);
    if (tokens >= 1.0) {
      tokens -= 1.0;
      return true;
    }
    return false;
  }
  uint64_t wait_ns(uint64_t now) {
    refill(now);
    if (tokens >= 1.0) return 0;
    return static_cast<uint64_t>((1.0 - tokens) / max_rps * 1e9) + 1000;
  }
};

struct FeedPoller::Feed {
  FeedSpec spec;
  net::Url url;
  Host* host = nullptr;
  uint16_t id = 0;
  std::string etag;
  std::string last_modified;
  FlatSet64 seen{4096};
  bool primed = false;
  uint64_t backoff_until = 0;
  uint32_t backoff_streak = 0;
  FeedStats st;
};

struct FeedPoller::Lane {
  IoTag tag{};
  Feed* feed = nullptr;
  std::unique_ptr<net::Connection> conn;
  uint64_t period_ns = 0;
  uint64_t next_fire = 0;
  uint64_t reconnect_at = 0;
  uint32_t fail_streak = 0;
  net::Buffer req{1024};
};

struct FeedPoller::JobLane {
  struct Job {
    std::string url;
    int stage = 0;  // 0 = filing index page, 1 = exhibit document
    std::unique_ptr<NewsEvent> base;
    uint64_t queued_mono = 0;
  };
  IoTag tag{};
  Host* host = nullptr;
  std::unique_ptr<net::Connection> conn;
  std::deque<Job> q;
  Job cur;
  bool inflight = false;
  uint64_t reconnect_at = 0;
  uint32_t fail_streak = 0;
  net::Buffer req{1024};
};

FeedPoller::FeedPoller(PollerOptions opts, std::vector<FeedSpec> feeds, std::vector<HostPolicy> hosts,
                       const SymbolTable& symbols, SpscQueue<NewsEvent>& out, Waker* out_waker)
    : opts_(std::move(opts)),
      specs_(std::move(feeds)),
      host_policies_(std::move(hosts)),
      symbols_(symbols),
      out_(out),
      out_waker_(out_waker) {}

FeedPoller::~FeedPoller() {
  lanes_.clear();
  job_lanes_.clear();
  if (ep_ >= 0) ::close(ep_);
}

const FeedSpec& FeedPoller::spec(std::size_t i) const { return feeds_[i]->spec; }
const FeedPoller::FeedStats& FeedPoller::stats(std::size_t i) const { return feeds_[i]->st; }

bool FeedPoller::init(std::string* err) {
  tls_ = std::make_unique<net::TlsContext>(opts_.ca_file, opts_.verify_tls);
  if (!tls_->ok()) {
    if (err) *err = tls_->error();
    return false;
  }
  ep_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_ < 0) {
    if (err) *err = "epoll_create1 failed";
    return false;
  }
  net::Connection::Options copts;
  copts.connect_timeout_ms = opts_.connect_timeout_ms;
  copts.io_timeout_ms = opts_.io_timeout_ms;

  auto host_for = [&](const net::Url& u) -> Host* {
    for (auto& h : hosts_)
      if (h->name == u.host && h->port == u.port && h->tls == u.tls) return h.get();
    auto h = std::make_unique<Host>();
    h->name = u.host;
    h->port = u.port;
    h->tls = u.tls;
    h->max_rps = opts_.default_max_rps;
    for (const auto& p : host_policies_)
      if (p.host == u.host) h->max_rps = p.max_rps;
    h->burst = std::max(1.0, std::min(h->max_rps, 4.0));
    h->tokens = h->burst;
    std::string rerr;
    if (!resolver_.resolve(u.host, u.port, h->addr, &rerr))
      LOG_WARN("dns: cannot resolve %s yet (%s); will retry", u.host.c_str(), rerr.c_str());
    hosts_.push_back(std::move(h));
    return hosts_.back().get();
  };

  const uint64_t now = mono_ns();
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    auto u = net::parse_url(specs_[i].url);
    if (!u) {
      if (err) *err = "feed " + specs_[i].name + ": bad url " + specs_[i].url;
      return false;
    }
    auto f = std::make_unique<Feed>();
    f->spec = specs_[i];
    f->spec.lanes = std::max<uint32_t>(1, f->spec.lanes);
    f->spec.interval_ms = std::max<uint32_t>(50, f->spec.interval_ms);
    f->url = *u;
    f->host = host_for(*u);
    f->id = static_cast<uint16_t>(i);
    Feed* fp = f.get();
    feeds_.push_back(std::move(f));
    for (uint32_t k = 0; k < fp->spec.lanes; ++k) {
      auto lane = std::make_unique<Lane>();
      lane->feed = fp;
      lane->period_ns = static_cast<uint64_t>(fp->spec.interval_ms) * fp->spec.lanes * kNsPerMs;
      // Stagger lanes evenly across the period; first poll right away.
      lane->next_fire = now + static_cast<uint64_t>(fp->spec.interval_ms) * k * kNsPerMs;
      lane->conn = std::make_unique<net::Connection>(u->host, u->port, u->tls, tls_.get(), copts);
      lane->tag = IoTag{0, static_cast<uint32_t>(lanes_.size())};
      lane->conn->attach_epoll(ep_, &lane->tag);
      lanes_.push_back(std::move(lane));
    }
  }
  // Exhibit fetches go to the EDGAR host on their own warm connection, so
  // they never delay a feed poll.
  if (opts_.fetch_exhibits) {
    for (auto& f : feeds_) {
      if (f->spec.kind != FeedKind::EdgarAtom) continue;
      bool have = false;
      for (auto& jl : job_lanes_) have |= jl->host == f->host;
      if (have) continue;
      auto jl = std::make_unique<JobLane>();
      jl->host = f->host;
      jl->conn = std::make_unique<net::Connection>(f->url.host, f->url.port, f->url.tls, tls_.get(), copts);
      jl->tag = IoTag{1, static_cast<uint32_t>(job_lanes_.size())};
      jl->conn->attach_epoll(ep_, &jl->tag);
      job_lanes_.push_back(std::move(jl));
    }
  }
  resolver_.start_refresh(300);
  for (auto& lane : lanes_) start_connect(*lane->conn, *lane->feed->host, now, lane->reconnect_at, lane->fail_streak);
  for (auto& jl : job_lanes_) start_connect(*jl->conn, *jl->host, now, jl->reconnect_at, jl->fail_streak);
  last_stats_ = now;
  return true;
}

void FeedPoller::start_connect(net::Connection& c, Host& h, uint64_t now, uint64_t& reconnect_at,
                               uint32_t& fail_streak) {
  if (h.addr.len == 0 && !resolver_.get(h.name, h.port, h.addr)) {
    ++fail_streak;
    reconnect_at = now + std::min<uint64_t>(30'000, 250ull << std::min<uint32_t>(fail_streak, 7)) * kNsPerMs;
    return;
  }
  if (!c.connect(reinterpret_cast<const sockaddr*>(&h.addr.addr), h.addr.len, now)) {
    ++fail_streak;
    reconnect_at = now + std::min<uint64_t>(30'000, 100ull << std::min<uint32_t>(fail_streak, 8)) * kNsPerMs;
  }
}

void FeedPoller::service_lane(Lane& lane, uint64_t now) {
  net::Connection& c = *lane.conn;
  Feed& f = *lane.feed;
  const auto st = c.state();
  if (st == net::Connection::State::Idle || st == net::Connection::State::Failed) {
    if (now >= lane.reconnect_at) {
      // Pick up DNS refreshes on reconnect.
      resolver_.get(f.host->name, f.host->port, f.host->addr);
      start_connect(c, *f.host, now, lane.reconnect_at, lane.fail_streak);
    }
    return;
  }
  // Refresh a warm connection shortly before the server's keep-alive timeout
  // would close it, so the next poll never lands on a dying socket.
  if (c.ready() && c.server_keepalive_timeout_s > 1) {
    const uint64_t idle = now - c.t_last_activity;
    const uint64_t limit = (static_cast<uint64_t>(c.server_keepalive_timeout_s) * 1000 - 750) * kNsPerMs;
    if (idle > limit && lane.next_fire > now) {
      c.close("keep-alive refresh");
      start_connect(c, *f.host, now, lane.reconnect_at, lane.fail_streak);
      return;
    }
  }
  if (now < lane.next_fire) return;
  if (f.backoff_until > now) {
    lane.next_fire = f.backoff_until;
    return;
  }
  if (!c.ready()) {
    ++f.st.skipped_busy;
    lane.next_fire += lane.period_ns;
    if (lane.next_fire <= now) lane.next_fire = now + lane.period_ns;
    return;
  }
  if (!f.host->take(now)) {
    ++f.st.rate_limited;
    lane.next_fire = now + f.host->wait_ns(now);
    return;
  }
  lane.req.clear();
  net::RequestSpec rs;
  rs.user_agent = opts_.user_agent;
  rs.if_none_match = f.etag;
  rs.if_modified_since = f.last_modified;
  rs.extra_headers = f.spec.extra_headers;
  net::build_request(lane.req, f.url, rs);
  if (!c.send(lane.req.view(), false, now)) {
    ++f.st.net_errors;
    lane.reconnect_at = now;
  } else {
    ++f.st.requests;
  }
  lane.next_fire += lane.period_ns;
  if (lane.next_fire <= now) lane.next_fire = now + lane.period_ns;
}

void FeedPoller::on_lane_event(Lane& lane, net::Connection::Event ev, uint64_t now) {
  Feed& f = *lane.feed;
  net::Connection& c = *lane.conn;
  switch (ev) {
    case net::Connection::Event::Connected:
      lane.fail_streak = 0;
      ++f.st.connects;
      f.st.connect.record(now - c.t_connect_start);
      break;
    case net::Connection::Event::Response:
      on_response(lane, now);
      c.recycle();
      if (!c.ready()) lane.reconnect_at = now;  // server asked to close: reopen now
      break;
    case net::Connection::Event::Closed:
      if (c.closed_while_idle()) {
        lane.reconnect_at = now;  // routine idle close: reopen immediately
      } else {
        ++f.st.net_errors;
        ++lane.fail_streak;
        lane.reconnect_at = now + std::min<uint64_t>(30'000, 100ull << std::min<uint32_t>(lane.fail_streak, 8)) * kNsPerMs;
        if (lane.fail_streak <= 3 || lane.fail_streak % 20 == 0)
          LOG_WARN("feed %s: connection error: %s (streak %u)", f.spec.name.c_str(),
                   c.last_error() ? c.last_error() : "?", lane.fail_streak);
      }
      break;
    default:
      break;
  }
}

void FeedPoller::on_response(Lane& lane, uint64_t now) {
  Feed& f = *lane.feed;
  net::Connection& c = *lane.conn;
  const auto& r = c.response();
  if (c.t_first_byte) f.st.ttfb.record(c.t_first_byte - c.t_sent);
  f.st.total.record(c.t_done - c.t_sent);
  if (r.status == 304) {
    ++f.st.not_modified;
    return;
  }
  if (r.status != 200) {
    ++f.st.http_errors;
    if (r.status == 403 || r.status == 429 || r.status == 503) {
      // Being throttled: back off hard. For SEC, 403 means the fair-access
      // limit was exceeded or the User-Agent is missing contact info.
      f.backoff_streak = std::min<uint32_t>(f.backoff_streak + 1, 6);
      f.backoff_until = now + (10ull << f.backoff_streak) * kNsPerSec;
      LOG_ERROR("feed %s: HTTP %d, backing off %llus", f.spec.name.c_str(), r.status,
                static_cast<unsigned long long>((10ull << f.backoff_streak)));
    } else if (f.st.http_errors <= 3 || f.st.http_errors % 50 == 0) {
      LOG_WARN("feed %s: HTTP %d", f.spec.name.c_str(), r.status);
    }
    return;
  }
  f.backoff_streak = 0;
  ++f.st.ok;
  if (!r.etag.empty()) f.etag = r.etag;
  if (!r.last_modified.empty()) f.last_modified = r.last_modified;
  const std::string_view doc = c.payload();
  if (c.payload_error()) {
    ++f.st.http_errors;
    return;
  }
  f.st.bytes += doc.size();

  const uint64_t wall = wall_ns();
  const uint64_t mono = mono_ns();
  const uint64_t recv_wall = wall - (mono - c.t_done);
  const uint64_t sent_wall = wall - (mono - c.t_sent);
  const uint64_t t0 = mono;
  int emitted = 0;
  scan_feed(f.spec.kind, doc, [&](std::string_view item, uint64_t id) {
    if (f.seen.contains(id)) return;
    if (!f.primed && !f.spec.emit_backlog) {
      f.seen.insert(id);
      return;
    }
    NewsEvent* ev = out_.try_claim();
    if (!ev) {
      ++f.st.dropped;  // engine ring full: not marked seen, retried next poll
      return;
    }
    ev->reset();
    if (!fill_event(f.spec.kind, item, symbols_, *ev, scratch_)) {
      f.seen.insert(id);
      return;
    }
    ev->id_hash = id;
    ev->source = f.id;
    ev->recv_ns = recv_wall;
    ev->sent_ns = sent_wall;
    ev->parsed_ns = wall_ns();
    if (ev->kind == EventKind::Filing) maybe_queue_exhibit(*ev);
    out_.publish();
    f.seen.insert(id);
    ++emitted;
  });
  if (emitted && out_waker_) out_waker_->notify();
  if (!f.primed) {
    f.primed = true;
    LOG_INFO("feed %s: primed (%zu items known, %zu bytes)", f.spec.name.c_str(), f.seen.size(), doc.size());
  }
  f.st.new_items += static_cast<uint64_t>(emitted);
  f.st.parse.record(mono_ns() - t0);
}

void FeedPoller::maybe_queue_exhibit(const NewsEvent& ev) {
  if (job_lanes_.empty() || ev.n_tickers == 0 || ev.link.empty()) return;
  const FormClass cls = classify_form(ev.form);
  if (cls != FormClass::Form8K && cls != FormClass::Form6K) return;
  const SymbolInfo& s = symbols_[ev.tickers[0]];
  if (!is_exchange_listed(s.exchange)) return;
  if (s.shares_outstanding > 0 && s.shares_outstanding > opts_.exhibit_max_shares) return;
  // 8-Ks carrying only governance items rarely have a press release.
  constexpr uint32_t kBoring = (1u << 17) | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 31);
  if (cls == FormClass::Form8K && ev.items_mask && (ev.items_mask & ~kBoring) == 0) return;
  auto u = net::parse_url(ev.link.view());
  if (!u) return;
  for (auto& jl : job_lanes_) {
    if (jl->host->name != u->host) continue;
    if (jl->q.size() >= opts_.exhibit_queue_max) {
      ++exhibit_st_.overflow;
      jl->q.pop_back();
    }
    JobLane::Job j;
    j.url = std::string(ev.link.view());
    j.stage = 0;
    j.base = std::make_unique<NewsEvent>(ev);
    j.queued_mono = mono_ns();
    jl->q.push_back(std::move(j));
    ++exhibit_st_.queued;
    return;
  }
}

void FeedPoller::service_jobs(JobLane& jl, uint64_t now) {
  net::Connection& c = *jl.conn;
  const auto st = c.state();
  if (st == net::Connection::State::Idle || st == net::Connection::State::Failed) {
    if (now >= jl.reconnect_at) start_connect(c, *jl.host, now, jl.reconnect_at, jl.fail_streak);
    return;
  }
  if (jl.inflight || jl.q.empty() || !c.ready()) return;
  // Feed polls have priority: leave a token for them when the bucket is low.
  if (jl.host->wait_ns(now) != 0 || jl.host->tokens < std::min(1.5, jl.host->burst)) return;
  if (!jl.host->take(now)) return;
  jl.cur = std::move(jl.q.front());
  jl.q.pop_front();
  auto u = net::parse_url(jl.cur.url);
  if (!u) {
    ++exhibit_st_.errors;
    return;
  }
  jl.req.clear();
  net::RequestSpec rs;
  rs.user_agent = opts_.user_agent;
  net::build_request(jl.req, *u, rs);
  if (c.send(jl.req.view(), false, now)) {
    jl.inflight = true;
  } else {
    jl.q.push_front(std::move(jl.cur));
    jl.reconnect_at = now;
  }
}

void FeedPoller::on_job_event(JobLane& jl, net::Connection::Event ev, uint64_t now) {
  net::Connection& c = *jl.conn;
  switch (ev) {
    case net::Connection::Event::Connected:
      jl.fail_streak = 0;
      break;
    case net::Connection::Event::Response:
      jl.inflight = false;
      on_job_response(jl, now);
      c.recycle();
      if (!c.ready()) jl.reconnect_at = now;
      break;
    case net::Connection::Event::Closed:
      if (jl.inflight) {
        // Retry the job once on a fresh connection.
        jl.inflight = false;
        if (jl.cur.base && jl.cur.stage < 10) {
          jl.cur.stage += 10;  // mark as retried
          jl.q.push_front(std::move(jl.cur));
        } else {
          ++exhibit_st_.errors;
        }
      }
      if (c.closed_while_idle()) {
        jl.reconnect_at = now;
      } else {
        ++jl.fail_streak;
        jl.reconnect_at = now + std::min<uint64_t>(30'000, 100ull << std::min<uint32_t>(jl.fail_streak, 8)) * kNsPerMs;
      }
      break;
    default:
      break;
  }
}

void FeedPoller::on_job_response(JobLane& jl, uint64_t now) {
  net::Connection& c = *jl.conn;
  JobLane::Job job = std::move(jl.cur);
  const int stage = job.stage % 10;
  if (c.response().status != 200 || !job.base) {
    ++exhibit_st_.errors;
    return;
  }
  const std::string_view body = c.payload();
  if (stage == 0) {
    const std::string href = find_exhibit_href(body);
    if (href.empty()) {
      ++exhibit_st_.none;
      return;
    }
    auto base = net::parse_url(job.url);
    if (!base) return;
    job.url = net::resolve_href(*base, href);
    job.stage = 1;
    jl.q.push_front(std::move(job));  // finish this filing before starting another
    return;
  }
  NewsEvent* ev = out_.try_claim();
  if (!ev) {
    ++exhibit_st_.errors;
    return;
  }
  fill_exhibit_event(body, *job.base, job.url, *ev, scratch_);
  const uint64_t wall = wall_ns();
  ev->recv_ns = wall - (mono_ns() - c.t_done);
  ev->sent_ns = wall - (mono_ns() - c.t_sent);
  ev->parsed_ns = wall_ns();
  out_.publish();
  if (out_waker_) out_waker_->notify();
  ++exhibit_st_.fetched;
  exhibit_st_.latency.record(now - job.queued_mono);
}

void FeedPoller::check_timeouts(uint64_t now) {
  for (auto& lane : lanes_) {
    const auto ev = lane->conn->check_timeout(now);
    if (ev != net::Connection::Event::None) on_lane_event(*lane, ev, now);
  }
  for (auto& jl : job_lanes_) {
    const auto ev = jl->conn->check_timeout(now);
    if (ev != net::Connection::Event::None) on_job_event(*jl, ev, now);
  }
}

uint64_t FeedPoller::next_deadline(uint64_t now) const {
  uint64_t d = now + 20 * kNsPerMs;  // timeout checks
  for (const auto& lane : lanes_) {
    const auto st = lane->conn->state();
    if (st == net::Connection::State::Idle || st == net::Connection::State::Failed) d = std::min(d, lane->reconnect_at);
    else if (lane->conn->ready()) d = std::min(d, lane->next_fire);
  }
  for (const auto& jl : job_lanes_) {
    if (!jl->q.empty() && !jl->inflight) d = std::min(d, now + 5 * kNsPerMs);
    const auto st = jl->conn->state();
    if (st == net::Connection::State::Idle || st == net::Connection::State::Failed) d = std::min(d, jl->reconnect_at);
  }
  return d;
}

void FeedPoller::step(uint64_t max_wait_ns) {
  uint64_t now = mono_ns();
  for (auto& lane : lanes_) service_lane(*lane, now);
  for (auto& jl : job_lanes_) service_jobs(*jl, now);

  uint64_t wait = 0;
  if (!opts_.busy_poll) {
    const uint64_t d = next_deadline(now);
    wait = d > now ? d - now : 0;
    if (wait > max_wait_ns) wait = max_wait_ns;
  }
  epoll_event evs[64];
  timespec ts{static_cast<time_t>(wait / kNsPerSec), static_cast<long>(wait % kNsPerSec)};
  int n = epoll_pwait2(ep_, evs, 64, &ts, nullptr);
  if (n < 0 && errno == ENOSYS) n = epoll_wait(ep_, evs, 64, static_cast<int>((wait + kNsPerMs - 1) / kNsPerMs));
  now = mono_ns();
  for (int i = 0; i < n; ++i) {
    const IoTag* tag = static_cast<const IoTag*>(evs[i].data.ptr);
    if (tag->type == 0) {
      Lane& lane = *lanes_[tag->index];
      on_lane_event(lane, lane.conn->on_events(evs[i].events, now), now);
    } else {
      JobLane& jl = *job_lanes_[tag->index];
      on_job_event(jl, jl.conn->on_events(evs[i].events, now), now);
    }
  }
  if (now - last_timeout_check_ > 20 * kNsPerMs) {
    check_timeouts(now);
    last_timeout_check_ = now;
  }
}

void FeedPoller::run(const std::atomic<bool>& stop) {
  while (!stop.load(std::memory_order_relaxed)) {
    step(50 * kNsPerMs);
    const uint64_t now = mono_ns();
    if (opts_.stats_interval_s > 0 && now - last_stats_ > static_cast<uint64_t>(opts_.stats_interval_s) * kNsPerSec) {
      last_stats_ = now;
      const std::string rep = stats_report(true);
      LOG_INFO("feed stats:\n%s", rep.c_str());
    }
  }
  resolver_.stop();
}

std::string FeedPoller::stats_report(bool reset) {
  std::string out;
  char line[512];
  auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
  for (auto& f : feeds_) {
    FeedStats& s = f->st;
    std::snprintf(line, sizeof(line),
                  "  %-18s req=%llu 200=%llu 304=%llu http_err=%llu net_err=%llu new=%llu drop=%llu busy=%llu "
                  "rl=%llu | ttfb p50=%.1fms p99=%.1fms | total p50=%.1fms | parse p50=%.0fus max=%.0fus\n",
                  f->spec.name.c_str(), (unsigned long long)s.requests, (unsigned long long)s.ok,
                  (unsigned long long)s.not_modified, (unsigned long long)s.http_errors,
                  (unsigned long long)s.net_errors, (unsigned long long)s.new_items, (unsigned long long)s.dropped,
                  (unsigned long long)s.skipped_busy, (unsigned long long)s.rate_limited, ms(s.ttfb.percentile(50)),
                  ms(s.ttfb.percentile(99)), ms(s.total.percentile(50)),
                  static_cast<double>(s.parse.percentile(50)) / 1e3, static_cast<double>(s.parse.max()) / 1e3);
    out += line;
    if (reset) {
      s.requests = s.ok = s.not_modified = s.http_errors = s.net_errors = s.connects = 0;
      s.new_items = s.dropped = s.skipped_busy = s.rate_limited = s.bytes = 0;
      s.ttfb.reset();
      s.total.reset();
      s.parse.reset();
      s.connect.reset();
    }
  }
  if (!job_lanes_.empty()) {
    std::snprintf(line, sizeof(line), "  exhibits: queued=%llu fetched=%llu none=%llu errors=%llu overflow=%llu p50=%.0fms\n",
                  (unsigned long long)exhibit_st_.queued, (unsigned long long)exhibit_st_.fetched,
                  (unsigned long long)exhibit_st_.none, (unsigned long long)exhibit_st_.errors,
                  (unsigned long long)exhibit_st_.overflow, ms(exhibit_st_.latency.percentile(50)));
    out += line;
  }
  return out;
}

}  // namespace stok
