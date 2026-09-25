#include <zlib.h>

#include "check.hpp"
#include "net/http.hpp"
#include "net/url.hpp"

using namespace stok;
using namespace stok::net;

namespace {

std::string gzip(const std::string& in) {
  z_stream zs{};
  deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&zs, in.size()) + 32, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

// Feeds `wire` to a fresh parser in pieces of `step` bytes.
ResponseParser::Result feed_in_steps(ResponseParser& p, Buffer& buf, const std::string& wire, std::size_t step,
                                     bool eof_at_end = false) {
  p.reset();
  buf.clear();
  ResponseParser::Result r = ResponseParser::Result::NeedMore;
  for (std::size_t i = 0; i < wire.size(); i += step) {
    buf.append(wire.data() + i, std::min(step, wire.size() - i));
    r = p.feed(buf, false);
    if (r != ResponseParser::Result::NeedMore) return r;
  }
  if (eof_at_end) r = p.feed(buf, true);
  return r;
}

}  // namespace

TEST(url_parse_and_resolve) {
  auto u = parse_url("https://www.sec.gov/cgi-bin/browse-edgar?action=getcurrent&type=8-K");
  CHECK(u.has_value());
  CHECK(u->tls);
  CHECK_EQ(u->host, std::string("www.sec.gov"));
  CHECK_EQ(u->port, 443);
  CHECK_EQ(u->target, std::string("/cgi-bin/browse-edgar?action=getcurrent&type=8-K"));
  auto p = parse_url("http://127.0.0.1:8443/x#frag");
  CHECK(p && !p->tls && p->port == 8443 && p->target == "/x" && p->host_header == "127.0.0.1:8443");
  CHECK(!parse_url("ftp://x").has_value());
  CHECK_EQ(resolve_href(*u, "/Archives/a.htm"), std::string("https://www.sec.gov/Archives/a.htm"));
  auto base = parse_url("https://www.sec.gov/Archives/edgar/data/1/2/idx.htm");
  CHECK_EQ(resolve_href(*base, "ex99.htm"), std::string("https://www.sec.gov/Archives/edgar/data/1/2/ex99.htm"));
  CHECK_EQ(resolve_href(*base, "https://x.y/z"), std::string("https://x.y/z"));
}

TEST(http_request_has_conditional_headers) {
  Buffer b;
  RequestSpec rs;
  rs.user_agent = "stok test@example.com";
  rs.if_none_match = "\"abc\"";
  rs.if_modified_since = "Fri, 25 Sep 2026 12:00:00 GMT";
  build_request(b, *parse_url("https://example.com/feed.rss"), rs);
  const std::string_view v = b.view();
  CHECK(v.substr(0, 27) == "GET /feed.rss HTTP/1.1\r\nHos");
  CHECK(v.find("If-None-Match: \"abc\"\r\n") != std::string_view::npos);
  CHECK(v.find("If-Modified-Since: Fri, 25 Sep 2026 12:00:00 GMT\r\n") != std::string_view::npos);
  CHECK(v.find("Accept-Encoding: gzip") != std::string_view::npos);
  CHECK(v.substr(v.size() - 4) == "\r\n\r\n");
}

TEST(http_parser_content_length_any_split) {
  const std::string wire =
      "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nETag: \"v1\"\r\nLast-Modified: Fri, 25 Sep 2026 12:00:00 GMT\r\n"
      "Keep-Alive: timeout=5, max=100\r\n\r\nhello world";
  ResponseParser p;
  Buffer buf;
  for (std::size_t step : {1u, 2u, 7u, 64u, 4096u}) {
    CHECK(feed_in_steps(p, buf, wire, step) == ResponseParser::Result::Done);
    CHECK_EQ(p.status, 200);
    CHECK_EQ(std::string(p.body()), std::string("hello world"));
    CHECK_EQ(p.etag, std::string("\"v1\""));
    CHECK_EQ(p.keep_alive_timeout_s, 5);
    CHECK(p.keep_alive);
  }
}

TEST(http_parser_chunked_any_split) {
  const std::string wire =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: y\r\n\r\n";
  ResponseParser p;
  Buffer buf;
  for (std::size_t step : {1u, 3u, 5u, 1000u}) {
    CHECK(feed_in_steps(p, buf, wire, step) == ResponseParser::Result::Done);
    CHECK_EQ(std::string(p.body()), std::string("hello world"));
  }
}

TEST(http_parser_304_close_delimited_and_interim) {
  ResponseParser p;
  Buffer buf;
  CHECK(feed_in_steps(p, buf, "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\n\r\n", 8) == ResponseParser::Result::Done);
  CHECK_EQ(p.status, 304);
  CHECK(p.body().empty());

  CHECK(feed_in_steps(p, buf, "HTTP/1.0 200 OK\r\n\r\nuntil close", 4, true) == ResponseParser::Result::Done);
  CHECK_EQ(std::string(p.body()), std::string("until close"));
  CHECK(!p.keep_alive);

  CHECK(feed_in_steps(p, buf, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 3) ==
        ResponseParser::Result::Done);
  CHECK_EQ(p.status, 200);
  CHECK_EQ(std::string(p.body()), std::string("ok"));

  CHECK(feed_in_steps(p, buf, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 1\r\n\r\nx", 100) ==
        ResponseParser::Result::Done);
  CHECK(!p.keep_alive);
  CHECK(feed_in_steps(p, buf, "garbage\r\n\r\n", 100) == ResponseParser::Result::Error);
}

TEST(http_gzip_body_inflates) {
  std::string doc;
  for (int i = 0; i < 2000; ++i) doc += "<item><title>headline " + std::to_string(i) + "</title></item>\n";
  const std::string gz = gzip(doc);
  const std::string wire = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + std::to_string(gz.size()) +
                           "\r\n\r\n" + gz;
  ResponseParser p;
  Buffer buf;
  CHECK(feed_in_steps(p, buf, wire, 1500) == ResponseParser::Result::Done);
  CHECK(p.gzip);
  Inflater inf;
  Buffer out;
  CHECK(inf.inflate(p.body(), out));
  CHECK_EQ(std::string(out.view()), doc);
  // Reuse of the same inflater (no re-init per response).
  CHECK(inf.inflate(p.body(), out));
  CHECK_EQ(out.size(), doc.size());
}
