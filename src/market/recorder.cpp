#include "market/recorder.hpp"

#include <zlib.h>

#include <chrono>

#include "core/log.hpp"
#include "core/thread.hpp"

namespace stok {

bool ItchRecorder::start(const std::string& path, std::string* err) {
  // Append mode: a restart mid-day adds a new gzip member, which readers
  // (zlib's gzread) handle transparently.
  gz_ = gzopen(path.c_str(), "ab1");
  if (!gz_) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  gzbuffer(gz_, 1 << 20);
  path_ = path;
  running_ = true;
  thr_ = std::thread([this] { run(); });
  return true;
}

void ItchRecorder::stop() {
  if (!running_.exchange(false)) return;
  if (thr_.joinable()) thr_.join();
  if (gz_) {
    gzclose(gz_);
    gz_ = nullptr;
  }
}

void ItchRecorder::run() {
  Logger::set_thread_tag("recorder");
  set_current_thread_name("stok-recorder");
  auto drain = [&] {
    std::size_t n = 0;
    while (RawMsg* m = q_.front()) {
      const uint8_t len[2] = {0, m->len};
      gzwrite(gz_, len, 2);
      gzwrite(gz_, m->data, m->len);
      q_.pop();
      ++n;
    }
    if (n) written_.fetch_add(n, std::memory_order_relaxed);
    return n;
  };
  auto last_flush = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_acquire)) {
    if (drain() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush > std::chrono::seconds(5)) {
      gzflush(gz_, Z_SYNC_FLUSH);  // bounded loss if the process dies
      last_flush = now;
    }
  }
  drain();
}

}  // namespace stok
