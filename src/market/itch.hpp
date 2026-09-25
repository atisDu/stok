#pragma once

#include <cstdint>
#include <cstring>

#include "core/common.hpp"

// Nasdaq TotalView-ITCH 5.0 decoder (the exchange's official binary protocol).
//
// Every message starts with: type(1) stock_locate(2) tracking(2) timestamp(6,
// ns since midnight ET). Fields are big-endian at fixed offsets, so decoding
// is a switch on the type byte plus a few byte-swaps: no allocation, no
// branches beyond the dispatch. The handler is a template parameter, so calls
// inline into the dispatch.
namespace stok::itch {

STOK_ALWAYS_INLINE uint16_t be16(const uint8_t* p) noexcept {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
STOK_ALWAYS_INLINE uint32_t be32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
STOK_ALWAYS_INLINE uint64_t be64(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
STOK_ALWAYS_INLINE uint64_t be48(const uint8_t* p) noexcept {
  return (static_cast<uint64_t>(be16(p)) << 32) | be32(p + 2);
}
// 8-byte space-padded ASCII stock symbol, as a little-endian word (matches
// SymbolTable::make_key).
STOK_ALWAYS_INLINE uint64_t stock_key(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

// Expected length per message type (0 = unknown type).
constexpr uint16_t message_length(uint8_t type) noexcept {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default: return 0;
  }
}

// Handler interface (all members required; unused ones can be empty):
//   on_system_event(ts, code)
//   on_stock_directory(locate, ts, key, market_category, financial_status, round_lot)
//   on_trading_action(locate, ts, key, state, reason4)
//   on_reg_sho(locate, ts, action)
//   on_add(locate, ts, ref, side, shares, price)
//   on_executed(locate, ts, ref, shares)
//   on_executed_price(locate, ts, ref, shares, printable, price)
//   on_cancel(locate, ts, ref, shares)
//   on_delete(locate, ts, ref)
//   on_replace(locate, ts, old_ref, new_ref, shares, price)
//   on_trade(locate, ts, side, shares, key, price)            // non-cross ('P')
//   on_cross(locate, ts, shares, key, price, cross_type)      // 'Q'
//   on_broken(locate, ts, match)
// Prices are uint32 with 4 implied decimals.
//
// Returns false if the message is truncated or of unknown type.
template <typename H>
STOK_ALWAYS_INLINE bool decode(const uint8_t* p, std::size_t len, H& h) {
  if (STOK_UNLIKELY(len < 11)) return false;
  const uint8_t type = p[0];
  const uint16_t need = message_length(type);
  if (STOK_UNLIKELY(need == 0 || len < need)) return false;
  const uint16_t locate = be16(p + 1);
  const uint64_t ts = be48(p + 5);
  switch (type) {
    case 'A':
    case 'F':
      h.on_add(locate, ts, be64(p + 11), static_cast<char>(p[19]), be32(p + 20), be32(p + 32));
      return true;
    case 'E':
      h.on_executed(locate, ts, be64(p + 11), be32(p + 19));
      return true;
    case 'C':
      h.on_executed_price(locate, ts, be64(p + 11), be32(p + 19), p[31] == 'Y', be32(p + 32));
      return true;
    case 'X':
      h.on_cancel(locate, ts, be64(p + 11), be32(p + 19));
      return true;
    case 'D':
      h.on_delete(locate, ts, be64(p + 11));
      return true;
    case 'U':
      h.on_replace(locate, ts, be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31));
      return true;
    case 'P':
      h.on_trade(locate, ts, static_cast<char>(p[19]), be32(p + 20), stock_key(p + 24), be32(p + 32));
      return true;
    case 'Q':
      h.on_cross(locate, ts, be64(p + 11), stock_key(p + 19), be32(p + 27), static_cast<char>(p[39]));
      return true;
    case 'S':
      h.on_system_event(ts, static_cast<char>(p[11]));
      return true;
    case 'R':
      h.on_stock_directory(locate, ts, stock_key(p + 11), static_cast<char>(p[19]), static_cast<char>(p[20]),
                           be32(p + 21));
      return true;
    case 'H':
      h.on_trading_action(locate, ts, stock_key(p + 11), static_cast<char>(p[19]), reinterpret_cast<const char*>(p + 21));
      return true;
    case 'Y':
      h.on_reg_sho(locate, ts, static_cast<char>(p[19]));
      return true;
    case 'B':
      h.on_broken(locate, ts, be64(p + 11));
      return true;
    default:
      return true;  // known but unused (L, V, W, K, J, h, I, N, O)
  }
}

// ---- encoders (tests, benchmarks, synthetic feeds) ----
namespace enc {
inline void put16(uint8_t* p, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(p, &v, 2); }
inline void put32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }
inline void put64(uint8_t* p, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(p, &v, 8); }
inline void put48(uint8_t* p, uint64_t v) { put16(p, static_cast<uint16_t>(v >> 32)); put32(p + 2, static_cast<uint32_t>(v)); }
inline void put_stock(uint8_t* p, const char* sym) {
  std::memset(p, ' ', 8);
  for (int i = 0; i < 8 && sym[i]; ++i) p[i] = static_cast<uint8_t>(sym[i]);
}
inline std::size_t header(uint8_t* p, char type, uint16_t locate, uint64_t ts) {
  std::memset(p, 0, message_length(static_cast<uint8_t>(type)));
  p[0] = static_cast<uint8_t>(type);
  put16(p + 1, locate);
  put16(p + 3, 0);
  put48(p + 5, ts);
  return message_length(static_cast<uint8_t>(type));
}
inline std::size_t system_event(uint8_t* p, uint64_t ts, char code) {
  auto n = header(p, 'S', 0, ts);
  p[11] = static_cast<uint8_t>(code);
  return n;
}
inline std::size_t stock_directory(uint8_t* p, uint16_t locate, uint64_t ts, const char* sym, char cat = 'S') {
  auto n = header(p, 'R', locate, ts);
  put_stock(p + 11, sym);
  p[19] = static_cast<uint8_t>(cat);
  p[20] = 'N';
  put32(p + 21, 100);
  return n;
}
inline std::size_t trading_action(uint8_t* p, uint16_t locate, uint64_t ts, const char* sym, char state,
                                  const char* reason) {
  auto n = header(p, 'H', locate, ts);
  put_stock(p + 11, sym);
  p[19] = static_cast<uint8_t>(state);
  std::memset(p + 21, ' ', 4);
  for (int i = 0; i < 4 && reason[i]; ++i) p[21 + i] = static_cast<uint8_t>(reason[i]);
  return n;
}
inline std::size_t add(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                       const char* sym, uint32_t price) {
  auto n = header(p, 'A', locate, ts);
  put64(p + 11, ref);
  p[19] = static_cast<uint8_t>(side);
  put32(p + 20, shares);
  put_stock(p + 24, sym);
  put32(p + 32, price);
  return n;
}
inline std::size_t executed(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match) {
  auto n = header(p, 'E', locate, ts);
  put64(p + 11, ref);
  put32(p + 19, shares);
  put64(p + 23, match);
  return n;
}
inline std::size_t executed_price(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares,
                                  bool printable, uint32_t price) {
  auto n = header(p, 'C', locate, ts);
  put64(p + 11, ref);
  put32(p + 19, shares);
  put64(p + 23, 1);
  p[31] = printable ? 'Y' : 'N';
  put32(p + 32, price);
  return n;
}
inline std::size_t cancel(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
  auto n = header(p, 'X', locate, ts);
  put64(p + 11, ref);
  put32(p + 19, shares);
  return n;
}
inline std::size_t del(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t ref) {
  auto n = header(p, 'D', locate, ts);
  put64(p + 11, ref);
  return n;
}
inline std::size_t replace(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                           uint32_t shares, uint32_t price) {
  auto n = header(p, 'U', locate, ts);
  put64(p + 11, old_ref);
  put64(p + 19, new_ref);
  put32(p + 27, shares);
  put32(p + 31, price);
  return n;
}
inline std::size_t trade(uint8_t* p, uint16_t locate, uint64_t ts, char side, uint32_t shares, const char* sym,
                         uint32_t price) {
  auto n = header(p, 'P', locate, ts);
  put64(p + 11, 0);
  p[19] = static_cast<uint8_t>(side);
  put32(p + 20, shares);
  put_stock(p + 24, sym);
  put32(p + 32, price);
  put64(p + 36, 1);
  return n;
}
inline std::size_t cross(uint8_t* p, uint16_t locate, uint64_t ts, uint64_t shares, const char* sym, uint32_t price,
                         char type) {
  auto n = header(p, 'Q', locate, ts);
  put64(p + 11, shares);
  put_stock(p + 19, sym);
  put32(p + 27, price);
  put64(p + 31, 1);
  p[39] = static_cast<uint8_t>(type);
  return n;
}
}  // namespace enc

}  // namespace stok::itch
