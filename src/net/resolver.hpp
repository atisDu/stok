#pragma once

#include <sys/socket.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace stok::net {

struct Address {
  sockaddr_storage addr{};
  socklen_t len = 0;
};

// DNS cache with background refresh. getaddrinfo() can block for tens of ms,
// so the latency-critical thread never calls it after start-up: it reads the
// cache, and a helper thread refreshes entries off the hot path.
class Resolver {
 public:
  Resolver() = default;
  ~Resolver() { stop(); }

  // Blocking resolve + cache insert. Used at start-up and by the refresher.
  bool resolve(const std::string& host, uint16_t port, Address& out, std::string* err = nullptr);

  // Cached lookup; falls back to a blocking resolve on a miss.
  bool get(const std::string& host, uint16_t port, Address& out);

  void start_refresh(int interval_s);
  void stop();

 private:
  std::mutex mu_;
  std::unordered_map<std::string, Address> cache_;
  std::thread thr_;
  std::atomic<bool> running_{false};
  std::condition_variable cv_;
  std::mutex cv_mu_;
};

}  // namespace stok::net
