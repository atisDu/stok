#pragma once

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

#include "core/common.hpp"

namespace stok {

// Low-latency sleep/wake for a queue consumer, without spinning a core.
//
// The consumer announces it is about to sleep, re-checks its queues, then
// blocks on an eventfd. Producers only make the (expensive) write() syscall
// when the consumer is actually asleep, so under load there are no syscalls.
// Seq-cst fences on both sides (Dekker pattern) close the lost-wakeup race.
//
// When `spin` is set the consumer busy-polls instead (lowest latency, burns a
// core; use with isolcpus).
class Waker {
 public:
  Waker() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  ~Waker() {
    if (fd_ >= 0) ::close(fd_);
  }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;

  // Producer: call after publishing to the queue.
  STOK_ALWAYS_INLINE void notify() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_relaxed)) {
      const uint64_t one = 1;
      [[maybe_unused]] ssize_t r = ::write(fd_, &one, sizeof(one));
    }
  }

  // Consumer: `has_work` re-checks the queues after announcing sleep. Blocks
  // at most timeout_ns. Returns early when notified.
  template <typename HasWork>
  void wait(uint64_t timeout_ns, HasWork&& has_work) noexcept {
    sleeping_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!has_work()) {
      pollfd p{fd_, POLLIN, 0};
      timespec ts{static_cast<time_t>(timeout_ns / 1'000'000'000ull),
                  static_cast<long>(timeout_ns % 1'000'000'000ull)};
      ::ppoll(&p, 1, &ts, nullptr);
    }
    sleeping_.store(false, std::memory_order_relaxed);
    uint64_t drain;
    [[maybe_unused]] ssize_t r = ::read(fd_, &drain, sizeof(drain));
  }

  int fd() const noexcept { return fd_; }

 private:
  int fd_;
  alignas(kCacheLine) std::atomic<bool> sleeping_{false};
};

}  // namespace stok
