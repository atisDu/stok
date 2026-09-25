#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/common.hpp"
#include "market/board.hpp"
#include "market/itch.hpp"
#include "ref/symbols.hpp"

typedef struct gzFile_s* gzFile;

namespace stok {

// ---------------------------------------------------------------------------
// ITCH 5.0 file replay: Nasdaq's binary file format is a sequence of
// [2-byte big-endian length][message]. Reads .gz or plain files (zlib handles
// both) through a large buffer.
class ItchFileReader {
 public:
  struct Stats {
    uint64_t messages = 0, bad = 0, bytes = 0;
  };

  ~ItchFileReader();
  bool open(const std::string& path, std::string* err);

  // Decodes up to `max_messages` (0 = all). speed: 0 = as fast as possible,
  // 1.0 = real time, N = N times real time (paced on ITCH timestamps).
  // Returns false at end of file.
  template <typename H>
  bool run(H& h, const std::atomic<bool>& stop, double speed = 0.0, uint64_t max_messages = 0);

  const Stats& stats() const { return st_; }

 private:
  bool refill();

  gzFile gz_ = nullptr;
  std::vector<uint8_t> buf_;
  std::size_t pos_ = 0, len_ = 0;
  bool eof_ = false;
  Stats st_;
  uint64_t pace_first_ts_ = 0;
  uint64_t pace_start_mono_ = 0;
};

template <typename H>
bool ItchFileReader::run(H& h, const std::atomic<bool>& stop, double speed, uint64_t max_messages) {
  uint64_t n = 0;
  uint64_t last_paced_ts = 0;
  for (;;) {
    if (len_ - pos_ < 2 || len_ - pos_ < 2u + ((static_cast<uint16_t>(buf_[pos_]) << 8) | buf_[pos_ + 1])) {
      if (!refill()) return false;
      if (len_ - pos_ < 2) return false;
      const std::size_t need = 2u + ((static_cast<uint16_t>(buf_[pos_]) << 8) | buf_[pos_ + 1]);
      if (len_ - pos_ < need) return false;  // truncated file
    }
    const uint16_t mlen = static_cast<uint16_t>((buf_[pos_] << 8) | buf_[pos_ + 1]);
    const uint8_t* msg = buf_.data() + pos_ + 2;
    pos_ += 2u + mlen;
    ++st_.messages;
    st_.bytes += 2u + mlen;
    if (speed > 0.0 && mlen >= 11) {
      const uint64_t ts = itch::be48(msg + 5);
      if (pace_first_ts_ == 0) {
        pace_first_ts_ = ts;
        pace_start_mono_ = mono_ns();
      }
      if (ts - last_paced_ts > 200'000) {  // re-check the clock every 200us of feed time
        last_paced_ts = ts;
        const uint64_t due = pace_start_mono_ + static_cast<uint64_t>(static_cast<double>(ts - pace_first_ts_) / speed);
        while (mono_ns() < due) {
          if (stop.load(std::memory_order_relaxed)) return true;
          cpu_relax();
        }
      }
    }
    if (STOK_UNLIKELY(!itch::decode(msg, mlen, h))) ++st_.bad;
    if (max_messages && ++n >= max_messages) return true;
    if ((st_.messages & 0xFFFF) == 0 && stop.load(std::memory_order_relaxed)) return true;
  }
}

// ---------------------------------------------------------------------------
// MoldUDP64 multicast receiver (Nasdaq's real-time transport for ITCH).
// Batch receive with recvmmsg, optional busy-polling, sequence tracking and
// gap detection. Retransmission requests are not implemented: gaps are
// counted and logged, and the affected symbols self-heal on the next trade.
class MoldUdp64Receiver {
 public:
  struct Options {
    std::string group;       // multicast group, e.g. 233.54.12.111
    uint16_t port = 0;
    std::string iface_addr;  // local interface IP for the join (empty = any)
    int rcvbuf_bytes = 64 << 20;
    bool busy_poll = true;
  };
  struct Stats {
    uint64_t packets = 0, messages = 0, heartbeats = 0, gaps = 0, gap_messages = 0, dups = 0, bad = 0;
  };

  ~MoldUdp64Receiver();
  bool open(const Options& o, std::string* err);
  // Receives and decodes up to one batch of packets. Returns packets handled.
  template <typename H>
  int poll(H& h, int timeout_ms);
  // Decodes one MoldUDP64 packet (exposed for tests).
  template <typename H>
  void on_packet(const uint8_t* p, std::size_t n, H& h);
  const Stats& stats() const { return st_; }
  bool session_ended() const { return ended_; }

 private:
  static constexpr int kBatch = 64;
  static constexpr int kPktSize = 2048;
  int recv_batch(int timeout_ms);

  int fd_ = -1;
  bool busy_ = true;
  uint64_t expected_seq_ = 0;
  char session_[10] = {};
  bool ended_ = false;
  Stats st_;
  std::vector<uint8_t> bufs_;
  std::vector<unsigned> lens_;
};

template <typename H>
void MoldUdp64Receiver::on_packet(const uint8_t* p, std::size_t n, H& h) {
  if (n < 20) {
    ++st_.bad;
    return;
  }
  ++st_.packets;
  if (std::memcmp(session_, p, 10) != 0) {
    std::memcpy(session_, p, 10);  // new session: resync on this packet
    expected_seq_ = 0;
  }
  const uint64_t seq = itch::be64(p + 10);
  const uint16_t count = itch::be16(p + 18);
  if (count == 0) {
    ++st_.heartbeats;
    if (expected_seq_ == 0) expected_seq_ = seq;
    return;
  }
  if (count == 0xFFFF) {
    ended_ = true;
    return;
  }
  if (expected_seq_ == 0) expected_seq_ = seq;
  uint64_t skip = 0;
  if (seq > expected_seq_) {
    ++st_.gaps;
    st_.gap_messages += seq - expected_seq_;
  } else if (seq + count <= expected_seq_) {
    ++st_.dups;
    return;
  } else if (seq < expected_seq_) {
    skip = expected_seq_ - seq;  // partial overlap
  }
  std::size_t off = 20;
  for (uint16_t i = 0; i < count; ++i) {
    if (off + 2 > n) {
      ++st_.bad;
      break;
    }
    const uint16_t mlen = itch::be16(p + off);
    off += 2;
    if (off + mlen > n) {
      ++st_.bad;
      break;
    }
    if (i >= skip) {
      ++st_.messages;
      if (!itch::decode(p + off, mlen, h)) ++st_.bad;
    }
    off += mlen;
  }
  expected_seq_ = seq + count;
}

template <typename H>
int MoldUdp64Receiver::poll(H& h, int timeout_ms) {
  const int n = recv_batch(timeout_ms);
  for (int i = 0; i < n; ++i) on_packet(bufs_.data() + static_cast<std::size_t>(i) * kPktSize, lens_[static_cast<std::size_t>(i)], h);
  return n;
}

// ---------------------------------------------------------------------------
// Bridge: a tiny UDP text protocol so any other data source (a broker SDK,
// a vendor websocket, a replay script) can drive the board without linking
// into the daemon. One or more lines per datagram:
//   T <SYMBOL> <PRICE> <SIZE> [EPOCH_NS]      trade
//   H <SYMBOL> <STATE> [REASON]               trading state (H/P/Q/T)
class BridgeReceiver {
 public:
  struct Stats {
    uint64_t datagrams = 0, trades = 0, states = 0, bad = 0, unknown_symbol = 0;
  };
  ~BridgeReceiver();
  bool open(const std::string& bind_addr, uint16_t port, std::string* err);
  // Returns datagrams processed.
  int poll(const SymbolTable& symbols, MarketBoard& board, int timeout_ms,
           void (*on_state)(void*, uint32_t, char, const char*, int64_t) = nullptr, void* ctx = nullptr);
  // Parses one datagram (exposed for tests).
  void handle(std::string_view text, const SymbolTable& symbols, MarketBoard& board,
              void (*on_state)(void*, uint32_t, char, const char*, int64_t), void* ctx);
  const Stats& stats() const { return st_; }

 private:
  int fd_ = -1;
  Stats st_;
};

}  // namespace stok
