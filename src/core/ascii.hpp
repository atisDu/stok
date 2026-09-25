#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "core/common.hpp"

namespace stok {

namespace detail {
constexpr std::array<unsigned char, 256> make_lower() {
  std::array<unsigned char, 256> t{};
  for (int i = 0; i < 256; ++i) t[i] = static_cast<unsigned char>((i >= 'A' && i <= 'Z') ? i + 32 : i);
  return t;
}
constexpr std::array<unsigned char, 256> make_upper() {
  std::array<unsigned char, 256> t{};
  for (int i = 0; i < 256; ++i) t[i] = static_cast<unsigned char>((i >= 'a' && i <= 'z') ? i - 32 : i);
  return t;
}
inline constexpr auto kLower = make_lower();
inline constexpr auto kUpper = make_upper();
}  // namespace detail

STOK_ALWAYS_INLINE constexpr char to_lower(char c) noexcept {
  return static_cast<char>(detail::kLower[static_cast<unsigned char>(c)]);
}
STOK_ALWAYS_INLINE constexpr char to_upper(char c) noexcept {
  return static_cast<char>(detail::kUpper[static_cast<unsigned char>(c)]);
}
STOK_ALWAYS_INLINE constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
STOK_ALWAYS_INLINE constexpr bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
STOK_ALWAYS_INLINE constexpr bool is_alpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
STOK_ALWAYS_INLINE constexpr bool is_alnum(char c) noexcept { return is_alpha(c) || is_digit(c); }
STOK_ALWAYS_INLINE constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0, e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

inline bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (to_lower(a[i]) != to_lower(b[i])) return false;
  return true;
}

inline bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

// Case-insensitive find. `needle` must be lowercase.
inline std::size_t ifind(std::string_view hay, std::string_view needle, std::size_t from = 0) noexcept {
  constexpr auto npos = std::string_view::npos;
  if (needle.empty()) return from <= hay.size() ? from : npos;
  if (hay.size() < needle.size() || from > hay.size() - needle.size()) return npos;
  const std::size_t last = hay.size() - needle.size();
  const char c0 = needle[0];
  for (std::size_t i = from; i <= last; ++i) {
    if (to_lower(hay[i]) != c0) continue;
    std::size_t k = 1;
    while (k < needle.size() && to_lower(hay[i + k]) == needle[k]) ++k;
    if (k == needle.size()) return i;
  }
  return npos;
}

template <typename T>
inline std::optional<T> parse_int(std::string_view s) noexcept {
  s = trim(s);
  T v{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

inline std::optional<double> parse_double(std::string_view s) noexcept {
  s = trim(s);
  if (!s.empty() && s[0] == '+') s.remove_prefix(1);
  double v{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// Splits on a single-character delimiter without allocating.
template <typename F>
inline void split(std::string_view s, char delim, F&& f) {
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = s.find(delim, start);
    if (pos == std::string_view::npos) {
      f(s.substr(start));
      return;
    }
    f(s.substr(start, pos - start));
    start = pos + 1;
  }
}

// Iterates lines (handles \n and \r\n).
template <typename F>
inline void for_each_line(std::string_view s, F&& f) {
  std::size_t start = 0;
  while (start < s.size()) {
    std::size_t nl = s.find('\n', start);
    if (nl == std::string_view::npos) nl = s.size();
    std::string_view line = s.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    f(line);
    start = nl + 1;
  }
}

inline std::string to_upper_copy(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = to_upper(c);
  return out;
}

inline std::string to_lower_copy(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = to_lower(c);
  return out;
}

}  // namespace stok
