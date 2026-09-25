#include "net/http_client.hpp"

#include <poll.h>
#include <sys/epoll.h>

#include "core/clock.hpp"

namespace stok::net {

HttpClient::HttpClient(TlsContext& tls, Resolver& resolver, std::string user_agent)
    : tls_(tls), resolver_(resolver), ua_(std::move(user_agent)) {}

Connection& HttpClient::conn_for(const Url& u) {
  auto& slot = conns_[u.key()];
  if (!slot) slot = std::make_unique<Connection>(u.host, u.port, u.tls, &tls_, Connection::Options{});
  return *slot;
}

// Waits for readiness per the connection's interest set and feeds events in.
Connection::Event HttpClient::pump(Connection& c, uint64_t deadline) {
  const uint64_t now = mono_ns();
  if (now >= deadline) return Connection::Event::Closed;
  if (c.fd() < 0) return Connection::Event::Closed;
  pollfd p{c.fd(), 0, 0};
  const uint32_t in = c.interest();
  if (in & EPOLLIN) p.events |= POLLIN;
  if (in & EPOLLOUT) p.events |= POLLOUT;
  if (in & EPOLLRDHUP) p.events |= POLLRDHUP;
  const int wait_ms = static_cast<int>((deadline - now) / 1'000'000ull) + 1;
  const int r = ::poll(&p, 1, wait_ms);
  const uint64_t t = mono_ns();
  if (r <= 0) {
    auto ev = c.check_timeout(t);
    if (ev == Connection::Event::Closed) return ev;
    return t >= deadline ? Connection::Event::Closed : Connection::Event::None;
  }
  uint32_t ev = 0;
  if (p.revents & POLLIN) ev |= EPOLLIN;
  if (p.revents & POLLOUT) ev |= EPOLLOUT;
  if (p.revents & POLLRDHUP) ev |= EPOLLRDHUP;
  if (p.revents & POLLHUP) ev |= EPOLLHUP;
  if (p.revents & POLLERR) ev |= EPOLLERR;
  return c.on_events(ev, t);
}

bool HttpClient::ensure_connected(Connection& c, const Url& u, uint64_t deadline, std::string& err) {
  if (c.ready()) {
    // Detect a peer close that happened while we were idle.
    pollfd p{c.fd(), POLLIN | POLLRDHUP, 0};
    if (::poll(&p, 1, 0) > 0) c.on_events(EPOLLIN | EPOLLRDHUP, mono_ns());
    if (c.ready()) return true;
  }
  Address addr;
  if (!resolver_.get(u.host, u.port, addr)) {
    err = "dns resolution failed for " + u.host;
    return false;
  }
  if (!c.connect(reinterpret_cast<const sockaddr*>(&addr.addr), addr.len, mono_ns())) {
    err = c.last_error() ? c.last_error() : "connect failed";
    return false;
  }
  while (!c.ready()) {
    const auto ev = pump(c, deadline);
    if (ev == Connection::Event::Closed || mono_ns() >= deadline) {
      err = c.last_error() ? c.last_error() : "connect timeout";
      c.close();
      return false;
    }
  }
  return true;
}

bool HttpClient::warm(const std::string& url, int timeout_ms) {
  auto u = parse_url(url);
  if (!u) return false;
  std::string err;
  return ensure_connected(conn_for(*u), *u, mono_ns() + static_cast<uint64_t>(timeout_ms) * 1'000'000ull, err);
}

HttpClient::Result HttpClient::request(std::string_view method, const std::string& url, std::string_view body,
                                       std::string_view content_type, std::string_view extra_headers,
                                       int timeout_ms) {
  Result res;
  const uint64_t t0 = mono_ns();
  const uint64_t deadline = t0 + static_cast<uint64_t>(timeout_ms) * 1'000'000ull;
  auto u = parse_url(url);
  if (!u) {
    res.error = "bad url: " + url;
    return res;
  }
  Connection& c = conn_for(*u);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool was_ready = c.ready();
    std::string err;
    const uint64_t tc = mono_ns();
    if (!ensure_connected(c, *u, deadline, err)) {
      res.error = err;
      return res;
    }
    res.reused = was_ready && c.ready() && c.requests_on_connection > 0;
    res.connect_ns = res.reused ? 0 : mono_ns() - tc;
    req_.clear();
    RequestSpec spec;
    spec.method = method;
    spec.user_agent = ua_;
    spec.extra_headers = extra_headers;
    spec.body = body;
    spec.content_type = content_type;
    build_request(req_, *u, spec);
    if (!c.send(req_.view(), method == "HEAD", mono_ns())) {
      if (attempt == 0 && res.reused) continue;  // stale keep-alive: retry fresh
      res.error = c.last_error() ? c.last_error() : "send failed";
      return res;
    }
    Connection::Event ev = Connection::Event::None;
    while (ev != Connection::Event::Response && ev != Connection::Event::Closed) ev = pump(c, deadline);
    if (ev == Connection::Event::Closed) {
      // A reused connection that died before any byte arrived is the classic
      // keep-alive race; retry once on a new connection.
      if (attempt == 0 && res.reused && c.t_first_byte == 0) continue;
      res.error = c.last_error() ? c.last_error() : "timeout";
      c.close();
      return res;
    }
    const auto& r = c.response();
    res.status = r.status;
    res.etag = r.etag;
    res.last_modified = r.last_modified;
    res.content_type = r.content_type;
    res.location = r.location;
    res.body.assign(c.payload());
    if (c.payload_error()) res.error = "decompression failed";
    res.wire_bytes = c.bytes_in;
    res.ttfb_ns = c.t_first_byte ? c.t_first_byte - c.t_sent : 0;
    res.total_ns = mono_ns() - t0;
    c.recycle();
    return res;
  }
  res.error = "request failed";
  return res;
}

HttpClient::Result HttpClient::get(const std::string& url, int timeout_ms, std::string_view extra_headers,
                                   int max_redirects) {
  std::string cur = url;
  for (int i = 0;; ++i) {
    Result r = request("GET", cur, {}, {}, extra_headers, timeout_ms);
    const bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 ||
                          r.status == 308;
    if (!redirect || r.location.empty() || i >= max_redirects) return r;
    auto base = parse_url(cur);
    if (!base) return r;
    cur = resolve_href(*base, r.location);
  }
}

HttpClient::Result HttpClient::post(const std::string& url, std::string_view body, std::string_view content_type,
                                    int timeout_ms, std::string_view extra_headers) {
  return request("POST", url, body, content_type, extra_headers, timeout_ms);
}

}  // namespace stok::net
