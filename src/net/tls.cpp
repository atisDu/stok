#include "net/tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace stok::net {

namespace {
int g_ctx_index = -1;
std::once_flag g_init;
}  // namespace

std::string TlsContext::last_error_string() {
  std::string out;
  unsigned long e;
  char buf[256];
  while ((e = ERR_get_error()) != 0) {
    ERR_error_string_n(e, buf, sizeof(buf));
    if (!out.empty()) out += "; ";
    out += buf;
  }
  return out;
}

TlsContext::TlsContext(const std::string& ca_file, bool verify_peer) : verify_peer_(verify_peer) {
  std::call_once(g_init, [] {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    g_ctx_index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  });
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ctx_) {
    error_ = "SSL_CTX_new failed: " + last_error_string();
    return;
  }
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  // Partial writes + moving buffers let us drive SSL_write from a non-blocking
  // state machine. IGNORE_UNEXPECTED_EOF treats a TCP FIN without close_notify
  // as a clean EOF (some servers delimit bodies that way).
  SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
  SSL_CTX_set_options(ctx_, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
  SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
  static const unsigned char kAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_CTX_set_alpn_protos(ctx_, kAlpn, sizeof(kAlpn));

  if (verify_peer_) {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    const int ok = ca_file.empty() ? SSL_CTX_set_default_verify_paths(ctx_)
                                   : SSL_CTX_load_verify_locations(ctx_, ca_file.c_str(), nullptr);
    if (ok != 1) {
      error_ = "loading CA certificates failed: " + last_error_string();
      return;
    }
  } else {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  }

  // Client-side session cache via callback: with TLS 1.3 tickets arrive after
  // the handshake, so SSL_get1_session() right after connect is too early.
  SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
  SSL_CTX_set_ex_data(ctx_, g_ctx_index, this);
  SSL_CTX_sess_set_new_cb(ctx_, &TlsContext::on_new_session);
}

TlsContext::~TlsContext() {
  for (auto& [host, s] : sessions_) SSL_SESSION_free(s);
  if (ctx_) SSL_CTX_free(ctx_);
}

int TlsContext::on_new_session(SSL* ssl, SSL_SESSION* sess) {
  auto* self = static_cast<TlsContext*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), g_ctx_index));
  const char* host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!self || !host) return 0;
  std::lock_guard<std::mutex> lk(self->mu_);
  auto& slot = self->sessions_[host];
  if (slot) SSL_SESSION_free(slot);
  slot = sess;  // we keep the reference OpenSSL handed us
  return 1;
}

void TlsContext::apply_session(SSL* ssl, const std::string& host) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = sessions_.find(host);
  if (it != sessions_.end() && it->second && SSL_SESSION_is_resumable(it->second)) SSL_set_session(ssl, it->second);
}

}  // namespace stok::net
