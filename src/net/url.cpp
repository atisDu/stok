#include "net/url.hpp"

#include "core/ascii.hpp"

namespace stok::net {

std::optional<Url> parse_url(std::string_view s) {
  s = trim(s);
  Url u;
  if (istarts_with(s, "https://")) {
    u.tls = true;
    u.port = 443;
    s.remove_prefix(8);
  } else if (istarts_with(s, "http://")) {
    u.tls = false;
    u.port = 80;
    s.remove_prefix(7);
  } else {
    return std::nullopt;
  }
  const std::size_t slash = s.find_first_of("/?");
  std::string_view authority = s.substr(0, slash);
  if (slash != std::string_view::npos) {
    u.target = std::string(s.substr(slash));
    if (u.target[0] == '?') u.target.insert(u.target.begin(), '/');
  }
  // Drop any fragment.
  if (const auto h = u.target.find('#'); h != std::string::npos) u.target.resize(h);
  if (authority.empty()) return std::nullopt;
  if (authority.find('@') != std::string_view::npos) return std::nullopt;  // no userinfo
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    auto p = parse_int<uint32_t>(authority.substr(colon + 1));
    if (!p || *p == 0 || *p > 65535) return std::nullopt;
    u.port = static_cast<uint16_t>(*p);
    authority = authority.substr(0, colon);
  }
  u.host = to_lower_copy(authority);
  const bool default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
  u.host_header = default_port ? u.host : u.host + ":" + std::to_string(u.port);
  return u;
}

std::string resolve_href(const Url& base, std::string_view href) {
  href = trim(href);
  if (istarts_with(href, "http://") || istarts_with(href, "https://")) return std::string(href);
  std::string origin = (base.tls ? "https://" : "http://") + base.host_header;
  if (href.size() >= 2 && href[0] == '/' && href[1] == '/') return (base.tls ? "https:" : "http:") + std::string(href);
  if (!href.empty() && href[0] == '/') return origin + std::string(href);
  // Relative to the base path's directory.
  std::string dir = base.target.substr(0, base.target.find('?'));
  const auto slash = dir.rfind('/');
  dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
  return origin + dir + std::string(href);
}

}  // namespace stok::net
