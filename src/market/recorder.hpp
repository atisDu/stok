#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/spsc_queue.hpp"

typedef struct gzFile_s* gzFile;

namespace stok {

// Records in-sequence ITCH messages to a Nasdaq-format file
// ([2-byte BE length][message]..., gzip) so every live session can be
// replayed by stok-backtest / stok-replay. The market thread only copies each
// message into a lock-free ring (~10 ns); compression and disk I/O happen on
// the recorder's own thread. If the ring overflows, messages are dropped and
// counted rather than stalling the feed.
class ItchRecorder {
 public:
  struct RawMsg {
    uint8_t len;
    uint8_t data[63];  // longest ITCH 5.0 message is 50 bytes
  };

  ItchRecorder() : q_(1u << 20) {}
  ~ItchRecorder() { stop(); }

  bool start(const std::string& path, std::string* err);
  void stop();

  // Market thread.
  void push(const uint8_t* msg, std::size_t len) noexcept {
    if (len > sizeof(RawMsg::data)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    RawMsg* s = q_.try_claim();
    if (!s) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s->len = static_cast<uint8_t>(len);
    std::memcpy(s->data, msg, len);
    q_.publish();
  }

  uint64_t written() const { return written_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  const std::string& path() const { return path_; }

 private:
  void run();

  SpscQueue<RawMsg> q_;
  gzFile gz_ = nullptr;
  std::string path_;
  std::thread thr_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> written_{0};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace stok
