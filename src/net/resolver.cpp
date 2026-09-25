#include "net/resolver.hpp"

#include <netdb.h>

#include <cstring>
#include <vector>

#include "core/log.hpp"

namespace stok::net {

static std::string key_of(const std::string& host, uint16_t port) { return host + ":" + std::to_string(port); }

bool Resolver::resolve(const std::string& host, uint16_t port, Address& out, std::string* err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  const int rc = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
  if (rc != 0 || !res) {
    if (err) *err = gai_strerror(rc);
    return false;
  }
  // Prefer IPv4: most feed CDNs have better-peered v4 paths and it avoids
  // happy-eyeballs style fallbacks on hosts with broken v6.
  const addrinfo* pick = res;
  for (const addrinfo* a = res; a; a = a->ai_next) {
    if (a->ai_family == AF_INET) {
      pick = a;
      break;
    }
  }
  Address a;
  std::memcpy(&a.addr, pick->ai_addr, pick->ai_addrlen);
  a.len = static_cast<socklen_t>(pick->ai_addrlen);
  freeaddrinfo(res);
  {
    std::lock_guard<std::mutex> lk(mu_);
    cache_[key_of(host, port)] = a;
  }
  out = a;
  return true;
}

bool Resolver::get(const std::string& host, uint16_t port, Address& out) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key_of(host, port));
    if (it != cache_.end()) {
      out = it->second;
      return true;
    }
  }
  return resolve(host, port, out);
}

void Resolver::start_refresh(int interval_s) {
  if (running_.exchange(true)) return;
  thr_ = std::thread([this, interval_s] {
    Logger::set_thread_tag("dns");
    while (running_.load()) {
      {
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, std::chrono::seconds(interval_s), [this] { return !running_.load(); });
      }
      if (!running_.load()) break;
      std::vector<std::pair<std::string, uint16_t>> keys;
      {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, v] : cache_) {
          const auto colon = k.rfind(':');
          keys.emplace_back(k.substr(0, colon), static_cast<uint16_t>(std::stoi(k.substr(colon + 1))));
        }
      }
      for (auto& [h, p] : keys) {
        Address a;
        std::string err;
        if (!resolve(h, p, a, &err)) LOG_WARN("dns refresh failed for %s: %s", h.c_str(), err.c_str());
      }
    }
  });
}

void Resolver::stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (thr_.joinable()) thr_.join();
}

}  // namespace stok::net
