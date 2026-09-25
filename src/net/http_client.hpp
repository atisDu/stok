#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "net/buffer.hpp"
#include "net/connection.hpp"
#include "net/resolver.hpp"
#include "net/tls.hpp"

namespace stok::net {

// Blocking HTTP client built on the same non-blocking Connection, for
// reference-data downloads, probes and alert delivery. Keeps one warm
// keep-alive connection per origin, so repeat calls skip TCP+TLS setup.
class HttpClient {
 public:
  struct Result {
    int status = 0;
    std::string body;  // decompressed
    std::string etag;
    std::string last_modified;
    std::string content_type;
    std::string location;
    std::string error;
    uint64_t connect_ns = 0;  // 0 when a warm connection was reused
    uint64_t ttfb_ns = 0;     // request sent -> first response byte
    uint64_t total_ns = 0;    // call start -> response complete
    std::size_t wire_bytes = 0;
    bool reused = false;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
  };

  HttpClient(TlsContext& tls, Resolver& resolver, std::string user_agent);

  Result get(const std::string& url, int timeout_ms = 20000, std::string_view extra_headers = {},
             int max_redirects = 3);
  Result post(const std::string& url, std::string_view body, std::string_view content_type, int timeout_ms = 10000,
              std::string_view extra_headers = {});

  // Opens (or re-validates) the connection to an origin without sending a
  // request, so a later call skips the handshake.
  bool warm(const std::string& url, int timeout_ms = 5000);

  const std::string& user_agent() const { return ua_; }

 private:
  Result request(std::string_view method, const std::string& url, std::string_view body,
                 std::string_view content_type, std::string_view extra_headers, int timeout_ms);
  Connection& conn_for(const Url& u);
  bool ensure_connected(Connection& c, const Url& u, uint64_t deadline, std::string& err);
  Connection::Event pump(Connection& c, uint64_t deadline);

  TlsContext& tls_;
  Resolver& resolver_;
  std::string ua_;
  std::map<std::string, std::unique_ptr<Connection>> conns_;
  Buffer req_;
};

}  // namespace stok::net
