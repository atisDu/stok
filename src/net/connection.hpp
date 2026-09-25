#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "net/buffer.hpp"
#include "net/http.hpp"

typedef struct ssl_st SSL;

namespace stok::net {

class TlsContext;

// One persistent HTTP/1.1 connection (plain or TLS) driven as a non-blocking
// state machine. The owner feeds it readiness events (from epoll or poll) and
// timeouts; it never blocks and never allocates in steady state.
//
//   Idle -> Connecting -> Handshaking -> Ready <-> Writing -> Reading -> Done
//                                          ^                             |
//                                          +------- recycle() ----------+
//
// Latency-related details:
//  * TCP_NODELAY (no Nagle delay on the small request write)
//  * TCP_QUICKACK re-armed after every read (no delayed-ACK stalls)
//  * TLS session resumption through the shared TlsContext cache
//  * keep-alive reuse: a warm connection costs one round trip per poll
//  * the Keep-Alive timeout the server advertises is exposed so owners can
//    reconnect before the server drops an idle connection
class Connection {
 public:
  enum class State : uint8_t { Idle, Connecting, Handshaking, Ready, Writing, Reading, Done, Failed };
  enum class Event : uint8_t { None, Connected, Response, Closed };

  struct Options {
    int connect_timeout_ms = 3000;
    int io_timeout_ms = 8000;
    bool nodelay = true;
    bool quickack = true;
    int rcvbuf_bytes = 0;  // 0 = kernel default (autotuning)
  };

  Connection(std::string host, uint16_t port, bool tls, TlsContext* tls_ctx, Options opts);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Registers with epoll (optional). `tag` becomes epoll_event.data.ptr.
  void attach_epoll(int epfd, void* tag) {
    epfd_ = epfd;
    epoll_tag_ = tag;
  }

  bool connect(const sockaddr* sa, socklen_t len, uint64_t now_ns);
  Event on_events(uint32_t epoll_events, uint64_t now_ns);
  Event check_timeout(uint64_t now_ns);

  // Starts a request on a Ready connection. `request` must stay alive until
  // the write finishes (the owner's request buffer).
  bool send(std::string_view request, bool head, uint64_t now_ns);

  // After consuming a Done response: back to Ready if keep-alive, else close.
  void recycle();
  void close(const char* reason = nullptr);

  State state() const { return state_; }
  bool ready() const { return state_ == State::Ready; }
  bool busy() const { return state_ == State::Writing || state_ == State::Reading; }
  bool connecting() const { return state_ == State::Connecting || state_ == State::Handshaking; }
  int fd() const { return fd_; }
  uint32_t interest() const { return interest_; }
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }

  const ResponseParser& response() const { return parser_; }
  // Decompressed body (inflated lazily, once). Empty on inflate failure.
  std::string_view payload();
  bool payload_error() const { return payload_error_; }

  const char* last_error() const { return last_error_; }
  bool closed_while_idle() const { return idle_close_; }

  // Timings (monotonic ns).
  uint64_t t_connect_start = 0;
  uint64_t t_connected = 0;
  uint64_t t_sent = 0;
  uint64_t t_first_byte = 0;
  uint64_t t_done = 0;
  uint64_t t_last_activity = 0;
  uint64_t requests_on_connection = 0;
  uint64_t bytes_in = 0;
  bool session_reused = false;
  int server_keepalive_timeout_s = -1;

 private:
  Event fail(const char* why);
  Event finish_connect(uint64_t now);
  Event do_handshake(uint64_t now);
  Event do_write(uint64_t now);
  Event do_read(uint64_t now);
  Event drain_idle();
  void set_interest(uint32_t ev);

  std::string host_;
  uint16_t port_;
  bool tls_;
  TlsContext* tls_ctx_;
  Options opts_;

  int fd_ = -1;
  SSL* ssl_ = nullptr;
  State state_ = State::Idle;
  int epfd_ = -1;
  void* epoll_tag_ = nullptr;
  uint32_t interest_ = 0;
  bool registered_ = false;

  std::string_view wreq_;
  std::size_t wpos_ = 0;
  bool head_ = false;

  Buffer rbuf_{64 * 1024};
  ResponseParser parser_;
  Inflater inflater_;
  Buffer plain_;
  bool payload_ready_ = false;
  bool payload_error_ = false;

  const char* last_error_ = nullptr;
  bool idle_close_ = false;
};

}  // namespace stok::net
