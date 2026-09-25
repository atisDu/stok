#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace stok {

// Fixed-capacity inline string. Trivially copyable, so it can sit inside
// events that travel through lock-free rings with no heap allocation. Truncates
// on a UTF-8 code-point boundary.
template <std::size_t N>
struct FixedStr {
  static_assert(N > 1 && N < 65535);
  uint16_t len = 0;
  char buf[N];

  void clear() noexcept {
    len = 0;
    buf[0] = '\0';
  }

  void assign(std::string_view s) noexcept {
    std::size_t n = s.size() < N - 1 ? s.size() : N - 1;
    if (n < s.size()) {
      // Don't split a multi-byte UTF-8 sequence.
      while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(buf, s.data(), n);
    len = static_cast<uint16_t>(n);
    buf[n] = '\0';
  }

  // Appends as much as fits.
  void append(std::string_view s) noexcept {
    std::size_t room = (N - 1) - len;
    std::size_t n = s.size() < room ? s.size() : room;
    if (n < s.size()) {
      while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(buf + len, s.data(), n);
    len = static_cast<uint16_t>(len + n);
    buf[len] = '\0';
  }

  std::string_view view() const noexcept { return {buf, len}; }
  const char* c_str() const noexcept { return buf; }
  bool empty() const noexcept { return len == 0; }
  static constexpr std::size_t capacity() noexcept { return N - 1; }
};

}  // namespace stok
