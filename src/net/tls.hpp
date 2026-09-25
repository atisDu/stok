#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_session_st SSL_SESSION;
typedef struct ssl_st SSL;

namespace stok::net {

// OpenSSL client context with a per-host session cache, so reconnects resume
// the TLS session (fewer CPU cycles and no certificate-chain transfer).
class TlsContext {
 public:
  // ca_file empty = system trust store (honors SSL_CERT_FILE / SSL_CERT_DIR).
  explicit TlsContext(const std::string& ca_file = "", bool verify_peer = true);
  ~TlsContext();
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  bool ok() const { return ctx_ != nullptr && error_.empty(); }
  const std::string& error() const { return error_; }
  SSL_CTX* get() const { return ctx_; }
  bool verify_peer() const { return verify_peer_; }

  // Sets a cached session (if any) on `ssl` before the handshake.
  void apply_session(SSL* ssl, const std::string& host);

  static std::string last_error_string();

 private:
  static int on_new_session(SSL* ssl, SSL_SESSION* sess);

  SSL_CTX* ctx_ = nullptr;
  bool verify_peer_ = true;
  std::string error_;
  std::mutex mu_;
  std::unordered_map<std::string, SSL_SESSION*> sessions_;
};

}  // namespace stok::net
