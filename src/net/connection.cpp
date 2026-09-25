#include "net/connection.hpp"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "net/tls.hpp"

namespace stok::net {

Connection::Connection(std::string host, uint16_t port, bool tls, TlsContext* tls_ctx, Options opts)
    : host_(std::move(host)), port_(port), tls_(tls), tls_ctx_(tls_ctx), opts_(opts) {}

Connection::~Connection() { close(); }

void Connection::set_interest(uint32_t ev) {
  if (fd_ < 0) return;
  if (registered_ && ev == interest_) return;
  interest_ = ev;
  if (epfd_ < 0) return;
  epoll_event e{};
  e.events = ev;
  e.data.ptr = epoll_tag_;
  if (!registered_) {
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd_, &e) == 0) registered_ = true;
  } else {
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd_, &e);
  }
}

void Connection::close(const char* reason) {
  if (reason) last_error_ = reason;
  if (ssl_) {
    // No close_notify round trip: we are dropping the connection anyway.
    SSL_set_quiet_shutdown(ssl_, 1);
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (fd_ >= 0) {
    if (registered_ && epfd_ >= 0) epoll_ctl(epfd_, EPOLL_CTL_DEL, fd_, nullptr);
    ::close(fd_);
    fd_ = -1;
  }
  registered_ = false;
  interest_ = 0;
  state_ = State::Idle;
}

Connection::Event Connection::fail(const char* why) {
  const bool was_idle = state_ == State::Ready;
  close(why);
  state_ = State::Failed;
  idle_close_ = was_idle;
  return Event::Closed;
}

bool Connection::connect(const sockaddr* sa, socklen_t len, uint64_t now_ns) {
  close();
  last_error_ = nullptr;
  idle_close_ = false;
  session_reused = false;
  requests_on_connection = 0;
  server_keepalive_timeout_s = -1;
  t_connect_start = now_ns;
  t_last_activity = now_ns;
  fd_ = ::socket(sa->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (fd_ < 0) {
    last_error_ = "socket() failed";
    state_ = State::Failed;
    return false;
  }
  int one = 1;
  if (opts_.nodelay) setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  if (opts_.rcvbuf_bytes > 0) setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &opts_.rcvbuf_bytes, sizeof(opts_.rcvbuf_bytes));
  const int r = ::connect(fd_, sa, len);
  if (r == 0) {
    state_ = State::Connecting;
    return finish_connect(now_ns) != Event::Closed;
  }
  if (errno != EINPROGRESS) {
    fail("connect() failed");
    return false;
  }
  state_ = State::Connecting;
  set_interest(EPOLLOUT);
  return true;
}

Connection::Event Connection::finish_connect(uint64_t now) {
  int err = 0;
  socklen_t elen = sizeof(err);
  if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) return fail("tcp connect failed");
  if (!tls_) {
    state_ = State::Ready;
    t_connected = now;
    set_interest(EPOLLIN | EPOLLRDHUP);
    return Event::Connected;
  }
  ssl_ = SSL_new(tls_ctx_->get());
  if (!ssl_) return fail("SSL_new failed");
  SSL_set_fd(ssl_, fd_);
  SSL_set_tlsext_host_name(ssl_, host_.c_str());
  if (tls_ctx_->verify_peer()) {
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set1_host(param, host_.c_str(), 0);
  }
  tls_ctx_->apply_session(ssl_, host_);
  SSL_set_connect_state(ssl_);
  state_ = State::Handshaking;
  return do_handshake(now);
}

Connection::Event Connection::do_handshake(uint64_t now) {
  ERR_clear_error();
  const int r = SSL_do_handshake(ssl_);
  if (r == 1) {
    state_ = State::Ready;
    t_connected = now;
    t_last_activity = now;
    session_reused = SSL_session_reused(ssl_) == 1;
    set_interest(EPOLLIN | EPOLLRDHUP);
    return Event::Connected;
  }
  switch (SSL_get_error(ssl_, r)) {
    case SSL_ERROR_WANT_READ:
      set_interest(EPOLLIN);
      return Event::None;
    case SSL_ERROR_WANT_WRITE:
      set_interest(EPOLLOUT);
      return Event::None;
    default:
      return fail("TLS handshake failed");
  }
}

bool Connection::send(std::string_view request, bool head, uint64_t now_ns) {
  if (state_ != State::Ready) return false;
  wreq_ = request;
  wpos_ = 0;
  head_ = head;
  parser_.reset(head);
  rbuf_.clear();
  payload_ready_ = false;
  payload_error_ = false;
  plain_.clear();
  t_sent = now_ns;
  t_first_byte = 0;
  t_done = 0;
  t_last_activity = now_ns;
  state_ = State::Writing;
  return do_write(now_ns) != Event::Closed;
}

Connection::Event Connection::do_write(uint64_t now) {
  while (wpos_ < wreq_.size()) {
    const char* p = wreq_.data() + wpos_;
    const std::size_t left = wreq_.size() - wpos_;
    if (tls_) {
      ERR_clear_error();
      const int n = SSL_write(ssl_, p, static_cast<int>(left));
      if (n > 0) {
        wpos_ += static_cast<std::size_t>(n);
        continue;
      }
      switch (SSL_get_error(ssl_, n)) {
        case SSL_ERROR_WANT_WRITE:
          set_interest(EPOLLOUT);
          return Event::None;
        case SSL_ERROR_WANT_READ:
          set_interest(EPOLLIN);
          return Event::None;
        default:
          return fail("SSL_write failed");
      }
    } else {
      const ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
      if (n > 0) {
        wpos_ += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        set_interest(EPOLLOUT);
        return Event::None;
      }
      return fail("send failed");
    }
  }
  t_last_activity = now;
  state_ = State::Reading;
  set_interest(EPOLLIN | EPOLLRDHUP);
  return Event::None;
}

Connection::Event Connection::do_read(uint64_t now) {
  bool eof = false;
  for (;;) {
    char* dst = rbuf_.prepare(32 * 1024);
    const std::size_t cap = rbuf_.spare();
    ssize_t n;
    if (tls_) {
      ERR_clear_error();
      const int r = SSL_read(ssl_, dst, static_cast<int>(cap > INT32_MAX ? INT32_MAX : cap));
      if (r > 0) {
        n = r;
      } else {
        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ) break;
        if (e == SSL_ERROR_WANT_WRITE) {
          set_interest(EPOLLOUT);
          break;
        }
        if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0)) {
          eof = true;
          break;
        }
        return fail("SSL_read failed");
      }
    } else {
      n = ::recv(fd_, dst, cap, 0);
      if (n == 0) {
        eof = true;
        break;
      }
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return fail("recv failed");
      }
    }
    if (t_first_byte == 0) t_first_byte = now;
    rbuf_.commit(static_cast<std::size_t>(n));
    bytes_in += static_cast<uint64_t>(n);
  }
  if (opts_.quickack && fd_ >= 0) {
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
  }
  t_last_activity = now;
  const auto r = parser_.feed(rbuf_, eof);
  if (r == ResponseParser::Result::Done) {
    t_done = now;
    ++requests_on_connection;
    if (parser_.keep_alive_timeout_s > 0) server_keepalive_timeout_s = parser_.keep_alive_timeout_s;
    state_ = State::Done;
    if (eof) parser_.keep_alive = false;
    return Event::Response;
  }
  if (r == ResponseParser::Result::Error) return fail(parser_.error ? parser_.error : "bad response");
  if (eof) return fail("connection closed mid-response");
  if (state_ == State::Reading) set_interest(EPOLLIN | EPOLLRDHUP);
  return Event::None;
}

// Readable while idle: either TLS 1.3 session tickets, a server-side close, or
// junk. Tickets are consumed silently; anything else ends the connection.
Connection::Event Connection::drain_idle() {
  char tmp[512];
  for (;;) {
    ssize_t n;
    if (tls_) {
      ERR_clear_error();
      const int r = SSL_read(ssl_, tmp, sizeof(tmp));
      if (r > 0) return fail("unexpected data on idle connection");
      const int e = SSL_get_error(ssl_, r);
      if (e == SSL_ERROR_WANT_READ) return Event::None;
      return fail("server closed idle connection");
    }
    n = ::recv(fd_, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Event::None;
    if (n > 0) return fail("unexpected data on idle connection");
    return fail("server closed idle connection");
  }
}

Connection::Event Connection::on_events(uint32_t ev, uint64_t now_ns) {
  if (fd_ < 0) return Event::None;
  switch (state_) {
    case State::Connecting:
      return finish_connect(now_ns);
    case State::Handshaking:
      return do_handshake(now_ns);
    case State::Writing:
      if (ev & (EPOLLERR | EPOLLHUP)) return fail("socket error while writing");
      return do_write(now_ns);
    case State::Reading:
      if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) return do_read(now_ns);
      if (ev & EPOLLOUT) return do_read(now_ns);  // SSL wanted a write
      return Event::None;
    case State::Ready:
      if (ev & EPOLLERR) return fail("socket error while idle");
      if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) return drain_idle();
      return Event::None;
    default:
      return Event::None;
  }
}

Connection::Event Connection::check_timeout(uint64_t now_ns) {
  switch (state_) {
    case State::Connecting:
    case State::Handshaking:
      if (now_ns - t_connect_start > static_cast<uint64_t>(opts_.connect_timeout_ms) * 1'000'000ull)
        return fail("connect timeout");
      break;
    case State::Writing:
    case State::Reading:
      if (now_ns - t_last_activity > static_cast<uint64_t>(opts_.io_timeout_ms) * 1'000'000ull)
        return fail("request timeout");
      break;
    default:
      break;
  }
  return Event::None;
}

void Connection::recycle() {
  if (state_ != State::Done) return;
  if (parser_.keep_alive && fd_ >= 0) {
    state_ = State::Ready;
    set_interest(EPOLLIN | EPOLLRDHUP);
  } else {
    close("server requested close");
  }
}

std::string_view Connection::payload() {
  if (!payload_ready_) {
    payload_ready_ = true;
    const std::string_view raw = parser_.body();
    if ((parser_.gzip || parser_.deflate) && !raw.empty()) {
      if (!inflater_.inflate(raw, plain_)) {
        payload_error_ = true;
        plain_.clear();
      }
    } else {
      plain_.clear();
      plain_.append(raw);
    }
  }
  return plain_.view();
}

}  // namespace stok::net
