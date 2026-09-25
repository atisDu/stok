#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/common.hpp"

namespace stok {

// splitmix64 finalizer: good avalanche for integer keys (order refs, CIKs).
STOK_ALWAYS_INLINE constexpr uint64_t mix64(uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

__extension__ typedef unsigned __int128 u128;

STOK_ALWAYS_INLINE uint64_t mum(uint64_t a, uint64_t b) noexcept {
  const u128 r = static_cast<u128>(a) * b;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}

STOK_ALWAYS_INLINE uint64_t load_u64_le(const void* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

// Fast 64-bit byte hash in the wyhash/mum family: 8 bytes per multiply. Used
// for story ids, title dedupe and symbol-key hashing. Stable across runs (no
// random seed) so hashes written to the journal can be compared across days.
inline uint64_t hash_bytes(const void* data, std::size_t n, uint64_t seed = 0x2d358dccaa6c78a5ull) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  uint64_t h = seed ^ mum(n, 0x9E3779B97F4A7C15ull);
  while (n >= 16) {
    h = mum(h ^ load_u64_le(p), 0xa0761d6478bd642full) ^ mum(load_u64_le(p + 8), 0xe7037ed1a0b428dbull);
    p += 16;
    n -= 16;
  }
  if (n >= 8) {
    h = mum(h ^ load_u64_le(p), 0xa0761d6478bd642full);
    p += 8;
    n -= 8;
  }
  uint64_t tail = 0;
  for (std::size_t i = 0; i < n; ++i) tail |= static_cast<uint64_t>(p[i]) << (8 * i);
  h = mum(h ^ tail, 0x8ebc6af09c88c6e3ull);
  return mix64(h);
}

inline uint64_t hash_sv(std::string_view s) noexcept { return hash_bytes(s.data(), s.size()); }

}  // namespace stok
