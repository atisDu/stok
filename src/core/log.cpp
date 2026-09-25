#include "core/log.hpp"

#include <chrono>
#include <cstring>
#include <ctime>

#include "core/clock.hpp"

namespace stok {

namespace {
thread_local char t_tag[16] = "main";
}

Logger& Logger::instance() {
  static Logger inst;
  return inst;
}

Logger::Logger() : q_(8192) {}

void Logger::set_thread_tag(const char* tag) {
  std::strncpy(t_tag, tag, sizeof(t_tag) - 1);
  t_tag[sizeof(t_tag) - 1] = '\0';
}

void Logger::start(const std::string& file_path, LogLevel min_level) {
  min_level_.store(min_level);
  if (!file_path.empty()) {
    FILE* f = std::fopen(file_path.c_str(), "a");
    if (f) {
      out_ = f;
      owns_out_ = true;
      std::setvbuf(out_, nullptr, _IOFBF, 1 << 16);
    }
  }
  bool expected = false;
  if (running_.compare_exchange_strong(expected, true)) thr_ = std::thread([this] { run(); });
}

void Logger::stop() {
  if (!running_.exchange(false)) {
    // Never started: flush anything queued synchronously.
    while (Record* r = q_.front()) {
      write_record(*r);
      q_.pop();
    }
    std::fflush(out_);
    return;
  }
  if (thr_.joinable()) thr_.join();
  if (owns_out_) {
    std::fclose(out_);
    out_ = stderr;
    owns_out_ = false;
  }
}

void Logger::log(LogLevel level, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(level, fmt, ap);
  va_end(ap);
}

void Logger::vlog(LogLevel level, const char* fmt, va_list ap) {
  const bool ok = q_.try_push_with([&](Record& r) {
    r.ts_ns = wall_ns();
    r.level = level;
    std::memcpy(r.thread, t_tag, sizeof(r.thread));
    va_list cp;
    va_copy(cp, ap);
    std::vsnprintf(r.msg, sizeof(r.msg), fmt, cp);
    va_end(cp);
  });
  if (!ok) dropped_.fetch_add(1, std::memory_order_relaxed);
  if (!running_.load(std::memory_order_relaxed)) {
    // Before start() / after stop(): write synchronously so nothing is lost.
    while (Record* r = q_.front()) {
      write_record(*r);
      q_.pop();
    }
    std::fflush(out_);
  }
}

void Logger::write_record(const Record& r) {
  static const char* kNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  const time_t secs = static_cast<time_t>(r.ts_ns / 1'000'000'000ull);
  const unsigned ms = static_cast<unsigned>((r.ts_ns / 1'000'000ull) % 1000);
  tm tmv;
  gmtime_r(&secs, &tmv);
  std::fprintf(out_, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ %s [%s] %s\n", tmv.tm_year + 1900, tmv.tm_mon + 1,
               tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, kNames[static_cast<int>(r.level) & 3],
               r.thread, r.msg);
}

void Logger::run() {
  set_thread_tag("log");
  uint64_t last_flush = mono_ns();
  for (;;) {
    bool any = false;
    while (Record* r = q_.front()) {
      write_record(*r);
      q_.pop();
      any = true;
    }
    const uint64_t now = mono_ns();
    if (any || now - last_flush > 200 * kNsPerMs) {
      std::fflush(out_);
      last_flush = now;
    }
    if (!running_.load(std::memory_order_acquire)) {
      while (Record* r = q_.front()) {
        write_record(*r);
        q_.pop();
      }
      std::fflush(out_);
      return;
    }
    if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace stok
