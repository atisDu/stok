#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "core/mpsc_queue.hpp"

namespace stok {

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Asynchronous logger. Callers format into a fixed slot of a lock-free MPSC
// ring (a snprintf, no syscall, no lock); a background thread does the I/O. If
// the ring is full the message is dropped and counted, so a logging burst
// never blocks a hot thread.
class Logger {
 public:
  struct Record {
    uint64_t ts_ns;
    LogLevel level;
    char thread[16];
    char msg[480];
  };

  static Logger& instance();

  void start(const std::string& file_path, LogLevel min_level);
  void stop();
  void set_level(LogLevel l) { min_level_.store(l, std::memory_order_relaxed); }
  bool enabled(LogLevel l) const { return l >= min_level_.load(std::memory_order_relaxed); }

  void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
  void vlog(LogLevel level, const char* fmt, va_list ap);

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  // Logs a header, then each line of `text` as its own record (records are
  // fixed-size, so long multi-line reports must not go in one record).
  void log_lines(LogLevel level, const char* header, const std::string& text);

  // Name used for records from the calling thread.
  static void set_thread_tag(const char* tag);

 private:
  Logger();
  void run();
  void write_record(const Record& r);

  MpscQueue<Record> q_;
  std::atomic<LogLevel> min_level_{LogLevel::Info};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> dropped_{0};
  std::thread thr_;
  FILE* out_ = stderr;
  bool owns_out_ = false;
};

}  // namespace stok

#define STOK_LOG(level, ...)                                                   \
  do {                                                                         \
    auto& stok_logger_ = ::stok::Logger::instance();                           \
    if (stok_logger_.enabled(level)) stok_logger_.log(level, __VA_ARGS__);     \
  } while (0)

#define LOG_DEBUG(...) STOK_LOG(::stok::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) STOK_LOG(::stok::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) STOK_LOG(::stok::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) STOK_LOG(::stok::LogLevel::Error, __VA_ARGS__)
