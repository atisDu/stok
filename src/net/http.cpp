#include "net/http.hpp"

#include <zlib.h>

#include <cstring>

#include "core/ascii.hpp"

namespace stok::net {

void build_request(Buffer& out, const Url& url, const RequestSpec& spec) {
  out.append(spec.method);
  out.append(" ");
  out.append(url.target);
  out.append(" HTTP/1.1\r\nHost: ");
  out.append(url.host_header);
  out.append("\r\n");
  if (!spec.user_agent.empty()) {
    out.append("User-Agent: ");
    out.append(spec.user_agent);
    out.append("\r\n");
  }
  out.append("Accept: */*\r\n");
  if (spec.accept_gzip) out.append("Accept-Encoding: gzip, deflate\r\n");
  out.append("Connection: keep-alive\r\n");
  if (!spec.if_none_match.empty()) {
    out.append("If-None-Match: ");
    out.append(spec.if_none_match);
    out.append("\r\n");
  }
  if (!spec.if_modified_since.empty()) {
    out.append("If-Modified-Since: ");
    out.append(spec.if_modified_since);
    out.append("\r\n");
  }
  out.append(spec.extra_headers);
  if (!spec.body.empty() || spec.method == "POST" || spec.method == "PUT") {
    if (!spec.content_type.empty()) {
      out.append("Content-Type: ");
      out.append(spec.content_type);
      out.append("\r\n");
    }
    out.append("Content-Length: ");
    out.append(std::to_string(spec.body.size()));
    out.append("\r\n");
  }
  out.append("\r\n");
  out.append(spec.body);
}

// ---------------------------------------------------------------------------

void ResponseParser::reset(bool head_request) {
  status = 0;
  keep_alive = true;
  keep_alive_timeout_s = -1;
  gzip = deflate = false;
  etag.clear();
  last_modified.clear();
  location.clear();
  content_type.clear();
  error = nullptr;
  phase_ = Phase::Headers;
  head_ = head_request;
  scan_ = 0;
  body_start_ = 0;
  content_length_ = -1;
  cs_ = ChunkState::Size;
  cpos_ = 0;
  chunk_left_ = 0;
  dechunked_.clear();
  use_dechunked_ = false;
  in_data_ = nullptr;
  body_len_ = 0;
}

std::string_view ResponseParser::body() const {
  if (use_dechunked_) return dechunked_.view();
  if (!in_data_) return {};
  return {in_data_ + body_start_, body_len_};
}

bool ResponseParser::parse_headers(std::string_view head) {
  // Status line: HTTP/1.x SSS reason
  const std::size_t eol = head.find("\r\n");
  std::string_view line = head.substr(0, eol);
  if (!istarts_with(line, "http/1.")) {
    error = "bad status line";
    return false;
  }
  const bool http10 = line.size() > 7 && line[7] == '0';
  keep_alive = !http10;
  const std::size_t sp = line.find(' ');
  if (sp == std::string_view::npos || sp + 4 > line.size()) {
    error = "bad status line";
    return false;
  }
  auto code = parse_int<int>(line.substr(sp + 1, 3));
  if (!code) {
    error = "bad status code";
    return false;
  }
  status = *code;

  bool chunked = false;
  std::size_t pos = eol == std::string_view::npos ? head.size() : eol + 2;
  while (pos < head.size()) {
    std::size_t e = head.find("\r\n", pos);
    if (e == std::string_view::npos) e = head.size();
    const std::string_view h = head.substr(pos, e - pos);
    pos = e + 2;
    const std::size_t colon = h.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view name = trim(h.substr(0, colon));
    const std::string_view value = trim(h.substr(colon + 1));
    switch (to_lower(name.empty() ? ' ' : name[0])) {
      case 'c':
        if (iequals(name, "content-length")) {
          auto n = parse_int<int64_t>(value);
          if (!n || *n < 0) {
            error = "bad content-length";
            return false;
          }
          content_length_ = *n;
        } else if (iequals(name, "content-encoding")) {
          if (ifind(value, "gzip") != std::string_view::npos) gzip = true;
          else if (ifind(value, "deflate") != std::string_view::npos) deflate = true;
        } else if (iequals(name, "connection")) {
          if (ifind(value, "close") != std::string_view::npos) keep_alive = false;
          else if (ifind(value, "keep-alive") != std::string_view::npos) keep_alive = true;
        } else if (iequals(name, "content-type")) {
          content_type.assign(value);
        }
        break;
      case 't':
        if (iequals(name, "transfer-encoding") && ifind(value, "chunked") != std::string_view::npos) chunked = true;
        break;
      case 'e':
        if (iequals(name, "etag")) etag.assign(value);
        break;
      case 'l':
        if (iequals(name, "last-modified")) last_modified.assign(value);
        else if (iequals(name, "location")) location.assign(value);
        break;
      case 'k':
        if (iequals(name, "keep-alive")) {
          const std::size_t t = ifind(value, "timeout=");
          if (t != std::string_view::npos) {
            std::size_t k = t + 8, b = k;
            while (k < value.size() && is_digit(value[k])) ++k;
            if (auto v = parse_int<int>(value.substr(b, k - b))) keep_alive_timeout_s = *v;
          }
        }
        break;
      default:
        break;
    }
  }

  if (head_ || status == 204 || status == 304 || (status >= 100 && status < 200)) {
    phase_ = Phase::Done;
  } else if (chunked) {
    phase_ = Phase::Chunked;
    use_dechunked_ = true;
  } else if (content_length_ >= 0) {
    phase_ = Phase::Length;
  } else {
    phase_ = Phase::UntilClose;
    keep_alive = false;
  }
  return true;
}

ResponseParser::Result ResponseParser::parse_chunked(const Buffer& in) {
  const std::string_view s = in.view();
  for (;;) {
    switch (cs_) {
      case ChunkState::Size: {
        const std::size_t e = s.find("\r\n", cpos_);
        if (e == std::string_view::npos) return Result::NeedMore;
        std::string_view line = s.substr(cpos_, e - cpos_);
        if (const auto semi = line.find(';'); semi != std::string_view::npos) line = line.substr(0, semi);
        line = trim(line);
        if (line.empty() || line.size() > 12) {
          error = "bad chunk size";
          return Result::Error;
        }
        std::size_t size = 0;
        for (char c : line) {
          size <<= 4;
          if (is_digit(c)) size |= static_cast<std::size_t>(c - '0');
          else if (c >= 'a' && c <= 'f') size |= static_cast<std::size_t>(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F') size |= static_cast<std::size_t>(c - 'A' + 10);
          else {
            error = "bad chunk size";
            return Result::Error;
          }
        }
        if (size > (512u << 20)) {
          error = "chunk too large";
          return Result::Error;
        }
        cpos_ = e + 2;
        if (size == 0) {
          cs_ = ChunkState::Trailer;
        } else {
          chunk_left_ = size;
          cs_ = ChunkState::Data;
        }
        break;
      }
      case ChunkState::Data: {
        const std::size_t avail = s.size() - cpos_;
        const std::size_t take = avail < chunk_left_ ? avail : chunk_left_;
        dechunked_.append(s.data() + cpos_, take);
        cpos_ += take;
        chunk_left_ -= take;
        if (chunk_left_ > 0) return Result::NeedMore;
        cs_ = ChunkState::DataCrlf;
        break;
      }
      case ChunkState::DataCrlf: {
        if (s.size() - cpos_ < 2) return Result::NeedMore;
        if (s[cpos_] != '\r' || s[cpos_ + 1] != '\n') {
          error = "missing chunk CRLF";
          return Result::Error;
        }
        cpos_ += 2;
        cs_ = ChunkState::Size;
        break;
      }
      case ChunkState::Trailer: {
        const std::size_t e = s.find("\r\n", cpos_);
        if (e == std::string_view::npos) return Result::NeedMore;
        const bool empty_line = e == cpos_;
        cpos_ = e + 2;
        if (empty_line) {
          phase_ = Phase::Done;
          return Result::Done;
        }
        break;
      }
    }
  }
}

ResponseParser::Result ResponseParser::feed(const Buffer& in, bool eof) {
  const std::string_view s = in.view();
  in_data_ = in.data();
  for (;;) {
    switch (phase_) {
      case Phase::Headers: {
        const std::size_t from = scan_ > 3 ? scan_ - 3 : 0;
        const std::size_t e = s.find("\r\n\r\n", from);
        if (e == std::string_view::npos) {
          scan_ = s.size();
          if (s.size() > (1u << 20)) {
            error = "headers too large";
            return Result::Error;
          }
          if (eof) {
            error = "eof in headers";
            return Result::Error;
          }
          return Result::NeedMore;
        }
        const std::size_t hdr_start = body_start_;  // non-zero after a 1xx
        if (!parse_headers(s.substr(hdr_start, e + 2 - hdr_start))) return Result::Error;
        body_start_ = e + 4;
        if (status >= 100 && status < 200 && status != 101) {
          // Interim response: parse the next header block.
          phase_ = Phase::Headers;
          scan_ = body_start_;
          continue;
        }
        cpos_ = body_start_;
        break;
      }
      case Phase::Length: {
        const std::size_t have = s.size() - body_start_;
        if (have < static_cast<std::size_t>(content_length_)) {
          if (eof) {
            error = "eof before content-length satisfied";
            return Result::Error;
          }
          return Result::NeedMore;
        }
        body_len_ = static_cast<std::size_t>(content_length_);
        phase_ = Phase::Done;
        return Result::Done;
      }
      case Phase::Chunked: {
        const Result r = parse_chunked(in);
        if (r == Result::NeedMore && eof) {
          error = "eof inside chunked body";
          return Result::Error;
        }
        return r;
      }
      case Phase::UntilClose: {
        if (!eof) return Result::NeedMore;
        body_len_ = s.size() - body_start_;
        phase_ = Phase::Done;
        return Result::Done;
      }
      case Phase::Done:
        return Result::Done;
    }
  }
}

// ---------------------------------------------------------------------------

Inflater::Inflater() : zs_(new z_stream{}) {}

Inflater::~Inflater() {
  if (current_bits_ != 0) inflateEnd(zs_);
  delete zs_;
}

bool Inflater::run(std::string_view in, Buffer& out, int window_bits) {
  if (current_bits_ == window_bits) {
    if (inflateReset(zs_) != Z_OK) return false;
  } else {
    if (current_bits_ != 0) inflateEnd(zs_);
    *zs_ = z_stream{};
    if (inflateInit2(zs_, window_bits) != Z_OK) {
      current_bits_ = 0;
      return false;
    }
    current_bits_ = window_bits;
  }
  out.clear();
  zs_->next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs_->avail_in = static_cast<uInt>(in.size());
  for (;;) {
    const std::size_t want = in.size() * 4 + 16384;
    char* dst = out.prepare(want);
    zs_->next_out = reinterpret_cast<Bytef*>(dst);
    zs_->avail_out = static_cast<uInt>(out.spare());
    const int r = ::inflate(zs_, Z_NO_FLUSH);
    out.commit(static_cast<std::size_t>(reinterpret_cast<char*>(zs_->next_out) - dst));
    if (r == Z_STREAM_END) {
      // Concatenated gzip members: continue with the next one.
      if (zs_->avail_in > 0 && window_bits > 15) {
        if (inflateReset(zs_) != Z_OK) return false;
        continue;
      }
      return true;
    }
    if (r == Z_BUF_ERROR && zs_->avail_in == 0) return true;  // truncated but usable
    if (r != Z_OK) return false;
  }
}

bool Inflater::inflate(std::string_view in, Buffer& out) {
  // 15+32: auto-detect gzip or zlib header. Fall back to raw deflate, which
  // some servers send for "Content-Encoding: deflate".
  if (run(in, out, 15 + 32)) return true;
  return run(in, out, -15);
}

}  // namespace stok::net
