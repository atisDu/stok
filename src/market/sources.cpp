#include "market/sources.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <cstring>

#include "core/ascii.hpp"
#include "util/time.hpp"

namespace stok {

// ---------------------------------------------------------------------------

ItchFileReader::~ItchFileReader() {
  if (gz_) gzclose(gz_);
}

bool ItchFileReader::open(const std::string& path, std::string* err) {
  gz_ = gzopen(path.c_str(), "rb");
  if (!gz_) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  gzbuffer(gz_, 1 << 20);
  buf_.resize(8 << 20);
  pos_ = len_ = 0;
  eof_ = false;
  return true;
}

bool ItchFileReader::refill() {
  if (eof_) return false;
  // Move the partial message to the front, then fill the rest.
  const std::size_t rem = len_ - pos_;
  if (rem && pos_) std::memmove(buf_.data(), buf_.data() + pos_, rem);
  pos_ = 0;
  len_ = rem;
  while (len_ < buf_.size()) {
    const int r = gzread(gz_, buf_.data() + len_, static_cast<unsigned>(buf_.size() - len_));
    if (r <= 0) {
      eof_ = true;
      break;
    }
    len_ += static_cast<std::size_t>(r);
  }
  return len_ > rem || rem >= 2;
}

// ---------------------------------------------------------------------------

MoldUdp64Receiver::~MoldUdp64Receiver() {
  if (fd_ >= 0) ::close(fd_);
}

bool MoldUdp64Receiver::open(const Options& o, std::string* err) {
  busy_ = o.busy_poll;
  bufs_.resize(static_cast<std::size_t>(kBatch) * kPktSize);
  lens_.resize(kBatch);
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    if (err) *err = "socket failed";
    return false;
  }
  int one = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &o.rcvbuf_bytes, sizeof(o.rcvbuf_bytes));
#ifdef SO_BUSY_POLL
  if (o.busy_poll) {
    int us = 50;
    setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us));
  }
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(o.port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind failed: " + std::string(std::strerror(errno));
    return false;
  }
  opts_ = o;
  if (!o.rerequest.empty()) {
    // Requests go out on the same socket, so the server's unicast replies
    // arrive through the normal receive path.
    const auto colon = o.rerequest.rfind(':');
    sockaddr_in rr{};
    rr.sin_family = AF_INET;
    if (colon == std::string::npos || inet_pton(AF_INET, o.rerequest.substr(0, colon).c_str(), &rr.sin_addr) != 1) {
      if (err) *err = "bad mold rerequest address " + o.rerequest;
      return false;
    }
    rr.sin_port = htons(static_cast<uint16_t>(std::stoi(o.rerequest.substr(colon + 1))));
    const int fd = fd_;
    request_fn_ = [fd, rr](const char* session, uint64_t seq, uint16_t count) {
      uint8_t req[20];
      std::memcpy(req, session, 10);
      itch::enc::put64(req + 10, seq);
      itch::enc::put16(req + 18, count);
      ::sendto(fd, req, sizeof(req), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&rr), sizeof(rr));
    };
  }
  if (!o.group.empty()) {
    ip_mreq mreq{};
    if (inet_pton(AF_INET, o.group.c_str(), &mreq.imr_multiaddr) != 1) {
      if (err) *err = "bad multicast group " + o.group;
      return false;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!o.iface_addr.empty() && inet_pton(AF_INET, o.iface_addr.c_str(), &mreq.imr_interface) != 1) {
      if (err) *err = "bad interface address " + o.iface_addr;
      return false;
    }
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
      if (err) *err = "IP_ADD_MEMBERSHIP failed: " + std::string(std::strerror(errno));
      return false;
    }
  }
  return true;
}

uint16_t MoldUdp64Receiver::local_port() const {
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  if (fd_ < 0 || getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
  return ntohs(a.sin_port);
}

void MoldUdp64Receiver::request_gap(uint64_t now) {
  if (!request_fn_) return;
  const uint64_t end = pending_.empty() ? gap_end_ : pending_.begin()->first;
  if (end <= expected_seq_) return;
  const uint64_t want = end - expected_seq_;
  const uint16_t count = static_cast<uint16_t>(want < opts_.max_request ? want : opts_.max_request);
  request_fn_(session_, expected_seq_, count);
  ++st_.requests;
  last_request_ = now;
}

int MoldUdp64Receiver::recv_batch(int timeout_ms) {
  mmsghdr msgs[kBatch];
  iovec iovs[kBatch];
  for (int i = 0; i < kBatch; ++i) {
    iovs[i].iov_base = bufs_.data() + static_cast<std::size_t>(i) * kPktSize;
    iovs[i].iov_len = kPktSize;
    msgs[i].msg_hdr = msghdr{};
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
  }
  int n = ::recvmmsg(fd_, msgs, kBatch, MSG_DONTWAIT, nullptr);
  if (n <= 0 && !busy_ && timeout_ms > 0) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) > 0) n = ::recvmmsg(fd_, msgs, kBatch, MSG_DONTWAIT, nullptr);
  }
  if (n <= 0) return 0;
  for (int i = 0; i < n; ++i) lens_[static_cast<std::size_t>(i)] = msgs[i].msg_len;
  return n;
}

// ---------------------------------------------------------------------------

BridgeReceiver::~BridgeReceiver() {
  if (fd_ >= 0) ::close(fd_);
}

bool BridgeReceiver::open(const std::string& bind_addr, uint16_t port, std::string* err) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    if (err) *err = "socket failed";
    return false;
  }
  int rcv = 8 << 20;
  setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_addr.empty() ? "127.0.0.1" : bind_addr.c_str(), &addr.sin_addr) != 1) {
    if (err) *err = "bad bind address";
    return false;
  }
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind failed: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
}

void BridgeReceiver::handle(std::string_view text, const SymbolTable& symbols, MarketBoard& board,
                            void (*on_state)(void*, uint32_t, char, const char*, int64_t), void* ctx,
                            int64_t default_ts) {
  ++st_.datagrams;
  const int64_t now_ts = default_ts ? default_ts : static_cast<int64_t>(wall_ns());
  for_each_line(text, [&](std::string_view line) {
    line = trim(line);
    if (line.empty()) return;
    std::string_view f[7];
    int n = 0;
    std::size_t i = 0;
    while (i < line.size() && n < 7) {
      while (i < line.size() && is_space(line[i])) ++i;
      const std::size_t b = i;
      while (i < line.size() && !is_space(line[i])) ++i;
      if (i > b) f[n++] = line.substr(b, i - b);
    }
    if (n < 3) {
      ++st_.bad;
      return;
    }
    const uint32_t sym = symbols.find(f[1]);
    if (sym == SymbolTable::kInvalid) {
      ++st_.unknown_symbol;
      return;
    }
    if (f[0] == "T" && n >= 4) {
      const auto px = parse_double(f[2]);
      const auto sz = parse_int<uint64_t>(f[3]);
      if (!px || !sz || *px <= 0) {
        ++st_.bad;
        return;
      }
      const int64_t ts = n >= 5 ? parse_int<int64_t>(f[4]).value_or(now_ts) : now_ts;
      const int64_t since_midnight = ts - board.midnight_ns();
      board.on_trade(sym, static_cast<int32_t>(*px * 1e4 + 0.5), *sz,
                     since_midnight > 0 ? static_cast<uint64_t>(since_midnight) : 0);
      ++st_.trades;
      if (tape_) std::fprintf(tape_, "%lld %.*s\n", static_cast<long long>(ts), static_cast<int>(line.size()), line.data());
    } else if (f[0] == "Q" && n >= 6) {
      const auto bid = parse_double(f[2]);
      const auto bsz = parse_int<uint64_t>(f[3]);
      const auto ask = parse_double(f[4]);
      const auto asz = parse_int<uint64_t>(f[5]);
      if (!bid || !bsz || !ask || !asz) {
        ++st_.bad;
        return;
      }
      const int64_t ts = n >= 7 ? parse_int<int64_t>(f[6]).value_or(now_ts) : now_ts;
      const int64_t since_midnight = ts - board.midnight_ns();
      board.on_quote(sym, static_cast<int32_t>(*bid * 1e4 + 0.5), *bsz, static_cast<int32_t>(*ask * 1e4 + 0.5), *asz,
                     since_midnight > 0 ? static_cast<uint64_t>(since_midnight) : 0);
      ++st_.quotes;
      if (tape_) std::fprintf(tape_, "%lld %.*s\n", static_cast<long long>(ts), static_cast<int>(line.size()), line.data());
    } else if (f[0] == "H") {
      char reason[5] = {' ', ' ', ' ', ' ', '\0'};
      if (n >= 4)
        for (std::size_t k = 0; k < 4 && k < f[3].size(); ++k) reason[k] = f[3][k];
      board.on_trading_action(sym, f[2][0], reason);
      if (on_state) on_state(ctx, sym, f[2][0], reason, now_ts);
      ++st_.states;
      if (tape_) std::fprintf(tape_, "%lld %.*s\n", static_cast<long long>(now_ts), static_cast<int>(line.size()), line.data());
    } else {
      ++st_.bad;
    }
  });
}

int BridgeReceiver::poll(const SymbolTable& symbols, MarketBoard& board, int timeout_ms,
                         void (*on_state)(void*, uint32_t, char, const char*, int64_t), void* ctx) {
  char buf[65536];
  int handled = 0;
  for (;;) {
    const ssize_t r = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
    if (r <= 0) {
      if (handled || timeout_ms <= 0) break;
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, timeout_ms) <= 0) break;
      timeout_ms = 0;
      continue;
    }
    handle(std::string_view(buf, static_cast<std::size_t>(r)), symbols, board, on_state, ctx);
    ++handled;
  }
  return handled;
}

}  // namespace stok
