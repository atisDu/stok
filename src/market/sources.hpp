#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/common.hpp"
#include "market/board.hpp"
#include "market/itch.hpp"
#include "market/recorder.hpp"
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
// Batch receive with recvmmsg, optional busy-polling and sequence tracking.
//
// Gap recovery: when packets go missing and a re-request server is configured,
// the receiver asks for the missing range (unicast MoldUDP64 request), buffers
// newer packets, and replays everything strictly in sequence once the gap is
// filled, so the order book never sees messages out of order. If the gap
// isn't filled within `gap_timeout_ms` it gives up, counts the lost messages
// and continues from the buffered packets. Without a re-request server, gaps
// are counted and skipped.
class MoldUdp64Receiver {
 public:
  struct Options {
    std::string group;       // multicast group, e.g. 233.54.12.111
    uint16_t port = 0;
    std::string iface_addr;  // local interface IP for the join (empty = any)
    int rcvbuf_bytes = 64 << 20;
    bool busy_poll = true;
    std::string rerequest;   // "ip:port" of the re-request server (empty = no recovery)
    int gap_timeout_ms = 250;
    int retry_ms = 50;
    uint16_t max_request = 1000;          // messages per request
    std::size_t max_buffered = 20000;     // packets held while a gap is open
  };
  struct Stats {
    uint64_t packets = 0, messages = 0, heartbeats = 0, gaps = 0, gap_messages = 0, dups = 0, bad = 0;
    uint64_t requests = 0, recovered = 0, gap_timeouts = 0, buffered_peak = 0;
  };
  using RequestFn = std::function<void(const char* session, uint64_t seq, uint16_t count)>;

  ~MoldUdp64Receiver();
  bool open(const Options& o, std::string* err);
  // Receives and decodes up to one batch of packets. Returns packets handled.
  template <typename H>
  int poll(H& h, int timeout_ms);
  // Handles one MoldUDP64 packet (exposed for tests).
  template <typename H>
  void on_packet(const uint8_t* p, std::size_t n, H& h, uint64_t now_ns = 0);
  // Gap timeouts and request retries. Call regularly.
  template <typename H>
  void tick(H& h, uint64_t now_ns);

  // Tests: enable recovery with a custom request sink (no socket).
  void enable_recovery(RequestFn fn, int gap_timeout_ms = 250, int retry_ms = 50, uint16_t max_request = 1000) {
    request_fn_ = std::move(fn);
    opts_.gap_timeout_ms = gap_timeout_ms;
    opts_.retry_ms = retry_ms;
    opts_.max_request = max_request;
  }
  // Every message applied to the book (in sequence, after gap recovery) is
  // also handed to the recorder.
  void set_recorder(ItchRecorder* r) { recorder_ = r; }
  const Stats& stats() const { return st_; }
  bool session_ended() const { return ended_; }
  bool recovering() const { return recovering_; }
  uint16_t local_port() const;
  uint64_t expected_seq() const { return expected_seq_; }

 private:
  static constexpr int kBatch = 64;
  static constexpr int kPktSize = 2048;
  int recv_batch(int timeout_ms);
  template <typename H>
  void process(const uint8_t* p, std::size_t n, H& h, uint64_t skip);
  template <typename H>
  void drain(H& h, uint64_t now);
  void request_gap(uint64_t now);

  Options opts_;
  int fd_ = -1;
  bool busy_ = true;
  uint64_t expected_seq_ = 0;
  char session_[10] = {};
  bool ended_ = false;
  bool recovering_ = false;
  uint64_t gap_since_ = 0;
  uint64_t gap_end_ = 0;  // first sequence known to exist after the gap
  uint64_t last_request_ = 0;
  std::map<uint64_t, std::vector<uint8_t>> pending_;  // seq -> raw packet (only during recovery)
  RequestFn request_fn_;
  ItchRecorder* recorder_ = nullptr;
  Stats st_;
  std::vector<uint8_t> bufs_;
  std::vector<unsigned> lens_;
};

template <typename H>
void MoldUdp64Receiver::process(const uint8_t* p, std::size_t n, H& h, uint64_t skip) {
  const uint16_t count = itch::be16(p + 18);
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
      if (recorder_) recorder_->push(p + off, mlen);
    }
    off += mlen;
  }
}

template <typename H>
void MoldUdp64Receiver::drain(H& h, uint64_t now) {
  while (!pending_.empty()) {
    auto it = pending_.begin();
    if (it->first > expected_seq_) break;
    const std::vector<uint8_t>& pkt = it->second;
    const uint16_t count = itch::be16(pkt.data() + 18);
    if (it->first + count > expected_seq_) {
      process(pkt.data(), pkt.size(), h, expected_seq_ - it->first);
      expected_seq_ = it->first + count;
    }
    pending_.erase(it);
  }
  if (pending_.empty()) {
    if (recovering_) ++st_.recovered;
    recovering_ = false;
  } else if (recovering_) {
    request_gap(now);  // the next hole in the buffered range
  }
}

template <typename H>
void MoldUdp64Receiver::on_packet(const uint8_t* p, std::size_t n, H& h, uint64_t now) {
  if (n < 20) {
    ++st_.bad;
    return;
  }
  if (now == 0) now = mono_ns();
  ++st_.packets;
  if (std::memcmp(session_, p, 10) != 0) {
    std::memcpy(session_, p, 10);  // new session: resync on this packet
    expected_seq_ = 0;
    pending_.clear();
    recovering_ = false;
  }
  const uint64_t seq = itch::be64(p + 10);
  const uint16_t count = itch::be16(p + 18);
  if (count == 0xFFFF) {
    ended_ = true;
    return;
  }
  if (count == 0) {
    // Heartbeat: its sequence is the next one the server will send.
    ++st_.heartbeats;
    if (expected_seq_ == 0) expected_seq_ = seq;
    else if (seq > expected_seq_ && request_fn_ && !recovering_) {
      recovering_ = true;
      gap_since_ = now;
      gap_end_ = seq;
      ++st_.gaps;
      last_request_ = 0;
      request_gap(now);
    }
    return;
  }
  if (expected_seq_ == 0) expected_seq_ = seq;
  if (seq + count <= expected_seq_) {
    ++st_.dups;
    return;
  }
  if (seq > expected_seq_) {
    if (!request_fn_) {  // no recovery: count and move on
      ++st_.gaps;
      st_.gap_messages += seq - expected_seq_;
      process(p, n, h, 0);
      expected_seq_ = seq + count;
      return;
    }
    if (pending_.size() < opts_.max_buffered) pending_.emplace(seq, std::vector<uint8_t>(p, p + n));
    if (pending_.size() > st_.buffered_peak) st_.buffered_peak = pending_.size();
    if (!recovering_) {
      recovering_ = true;
      gap_since_ = now;
      gap_end_ = seq;
      ++st_.gaps;
      last_request_ = 0;
      request_gap(now);
    }
    return;
  }
  process(p, n, h, expected_seq_ - seq);
  expected_seq_ = seq + count;
  drain(h, now);
}

template <typename H>
void MoldUdp64Receiver::tick(H& h, uint64_t now) {
  if (!recovering_) return;
  if (now - gap_since_ > static_cast<uint64_t>(opts_.gap_timeout_ms) * 1'000'000ull) {
    ++st_.gap_timeouts;
    if (pending_.empty()) {  // heartbeat-detected gap with nothing buffered
      recovering_ = false;
      return;
    }
    st_.gap_messages += pending_.begin()->first - expected_seq_;
    expected_seq_ = pending_.begin()->first;
    gap_since_ = now;
    drain(h, now);
    return;
  }
  if (now - last_request_ > static_cast<uint64_t>(opts_.retry_ms) * 1'000'000ull) request_gap(now);
}

template <typename H>
int MoldUdp64Receiver::poll(H& h, int timeout_ms) {
  const int n = recv_batch(recovering_ ? 0 : timeout_ms);
  const uint64_t now = mono_ns();
  for (int i = 0; i < n; ++i)
    on_packet(bufs_.data() + static_cast<std::size_t>(i) * kPktSize, lens_[static_cast<std::size_t>(i)], h, now);
  tick(h, now);
  return n;
}

// ---------------------------------------------------------------------------
// Bridge: a tiny UDP text protocol so any other data source (a broker SDK,
// a vendor websocket, a replay script) can drive the board without linking
// into the daemon. One or more lines per datagram:
//   T <SYMBOL> <PRICE> <SIZE> [EPOCH_NS]      trade
//   Q <SYMBOL> <BID> <BIDSIZE> <ASK> <ASKSIZE> top of book
//   H <SYMBOL> <STATE> [REASON]               trading state (H/P/Q/T)
class BridgeReceiver {
 public:
  struct Stats {
    uint64_t datagrams = 0, trades = 0, quotes = 0, states = 0, bad = 0, unknown_symbol = 0;
  };
  ~BridgeReceiver();
  bool open(const std::string& bind_addr, uint16_t port, std::string* err);
  // Returns datagrams processed.
  int poll(const SymbolTable& symbols, MarketBoard& board, int timeout_ms,
           void (*on_state)(void*, uint32_t, char, const char*, int64_t) = nullptr, void* ctx = nullptr);
  // Parses one datagram (exposed for tests).
  // `default_ts` (epoch ns) stamps lines that carry no timestamp; 0 = now.
  void handle(std::string_view text, const SymbolTable& symbols, MarketBoard& board,
              void (*on_state)(void*, uint32_t, char, const char*, int64_t), void* ctx, int64_t default_ts = 0);
  const Stats& stats() const { return st_; }
  // Tape recording: every valid line is appended as "<epoch_ns> <line>".
  void set_tape(FILE* f) { tape_ = f; }

 private:
  int fd_ = -1;
  FILE* tape_ = nullptr;
  Stats st_;
};

}  // namespace stok
