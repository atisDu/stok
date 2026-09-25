#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/histogram.hpp"
#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/signals.hpp"
#include "net/http_client.hpp"

namespace stok {

struct AlertOptions {
  bool stdout_enabled = true;
  std::string telegram_token;
  std::string telegram_chat_id;
  Tier telegram_min_tier = Tier::Alert;   // WATCH is journal-only unless lowered
  bool telegram_halts = true;
  bool telegram_movers = true;
  std::string udp_target;                 // "127.0.0.1:7788": one JSON datagram per signal
  std::string user_agent = "stok/0.1";
  std::string ca_file;
  bool verify_tls = true;
  int keepalive_ping_s = 25;              // keep the Telegram connection warm
};

// Delivers signals: stdout, Telegram and a local UDP JSON feed. The Telegram
// connection is opened at start-up and kept warm with periodic getMe calls,
// so an alert costs one request round trip, never a TCP+TLS handshake.
class AlertSink {
 public:
  AlertSink(AlertOptions opts, std::vector<std::string> source_names);
  ~AlertSink();

  bool init(std::string* err);
  void run(SpscQueue<Signal>& q, Waker& waker, const std::atomic<bool>& stop);
  void deliver(const Signal& s);

  static std::string format_text(const Signal& s, const std::vector<std::string>& sources);
  static std::string format_html(const Signal& s, const std::vector<std::string>& sources);

  struct Stats {
    uint64_t signals = 0, telegram_sent = 0, telegram_failed = 0, udp_sent = 0;
    LatencyHistogram signal_to_sent;  // engine emit -> Telegram 200 OK
  };
  const Stats& stats() const { return st_; }

 private:
  bool send_telegram(const Signal& s);
  void keepalive();
  void send_udp(const Signal& s);

  AlertOptions opts_;
  std::vector<std::string> sources_;
  std::unique_ptr<net::TlsContext> tls_;
  std::unique_ptr<net::Resolver> resolver_;
  std::unique_ptr<net::HttpClient> http_;
  int udp_fd_ = -1;
  std::vector<unsigned char> udp_addr_;
  uint64_t last_telegram_io_ = 0;
  uint64_t telegram_backoff_until_ = 0;
  Stats st_;
};

}  // namespace stok
