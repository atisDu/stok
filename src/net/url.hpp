#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace stok::net {

struct Url {
  bool tls = true;
  std::string host;
  uint16_t port = 443;
  std::string target = "/";  // path + query
  std::string host_header;   // host, or host:port for non-default ports

  std::string key() const { return host + ":" + std::to_string(port) + (tls ? "" : "/plain"); }
};

// Parses absolute http(s) URLs. Returns nullopt on anything else.
std::optional<Url> parse_url(std::string_view s);

// Resolves a possibly-relative href against a base URL.
std::string resolve_href(const Url& base, std::string_view href);

}  // namespace stok::net
