#pragma once

#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace stok::net {

// Growable byte buffer that is reused across requests: after warm-up a poll
// cycle allocates nothing. Unlike std::string, prepare()/commit() let
// recv/SSL_read write straight into spare capacity without zero-filling it.
class Buffer {
 public:
  Buffer() = default;
  explicit Buffer(std::size_t cap) { reserve(cap); }
  ~Buffer() { std::free(data_); }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& o) noexcept
      : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)), cap_(std::exchange(o.cap_, 0)) {}
  Buffer& operator=(Buffer&& o) noexcept {
    if (this != &o) {
      std::free(data_);
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
      cap_ = std::exchange(o.cap_, 0);
    }
    return *this;
  }

  char* data() noexcept { return data_; }
  const char* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return cap_; }
  bool empty() const noexcept { return size_ == 0; }
  std::string_view view() const noexcept { return {data_ ? data_ : "", size_}; }

  void clear() noexcept { size_ = 0; }

  void reserve(std::size_t n) {
    if (n <= cap_) return;
    std::size_t c = cap_ ? cap_ : 4096;
    while (c < n) c *= 2;
    char* p = static_cast<char*>(std::realloc(data_, c));
    if (!p) throw std::bad_alloc();
    data_ = p;
    cap_ = c;
  }

  // Returns a pointer to at least `n` writable bytes at the end.
  char* prepare(std::size_t n) {
    if (cap_ - size_ < n) reserve(size_ + n);
    return data_ + size_;
  }
  std::size_t spare() const noexcept { return cap_ - size_; }
  void commit(std::size_t n) noexcept { size_ += n; }

  void append(const void* p, std::size_t n) {
    if (n == 0) return;  // empty views may carry a null data pointer
    std::memcpy(prepare(n), p, n);
    size_ += n;
  }
  void append(std::string_view s) { append(s.data(), s.size()); }

  void erase_front(std::size_t n) noexcept {
    if (n >= size_) {
      size_ = 0;
      return;
    }
    std::memmove(data_, data_ + n, size_ - n);
    size_ -= n;
  }

 private:
  char* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t cap_ = 0;
};

}  // namespace stok::net
