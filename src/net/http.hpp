#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "net/buffer.hpp"
#include "net/url.hpp"

typedef struct z_stream_s z_stream;

namespace stok::net {

struct RequestSpec {
  std::string_view method = "GET";
  std::string_view user_agent;
  std::string_view if_none_match;      // ETag validator
  std::string_view if_modified_since;  // Last-Modified validator
  std::string_view extra_headers;      // preformatted "Name: value\r\n" lines
  std::string_view content_type;
  std::string_view body;
  bool accept_gzip = true;
};

// Serializes an HTTP/1.1 request into `out` (appends).
void build_request(Buffer& out, const Url& url, const RequestSpec& spec);

// Incremental HTTP/1.1 response parser. Call feed() with the whole receive
// buffer each time more bytes arrive; it resumes where it stopped (no
// rescanning). Handles Content-Length, chunked and read-until-close bodies,
// 1xx interim responses, HEAD/204/304, keep-alive negotiation and
// Keep-Alive: timeout=N.
class ResponseParser {
 public:
  enum class Result { NeedMore, Done, Error };

  void reset(bool head_request = false);
  Result feed(const Buffer& in, bool eof);

  int status = 0;
  bool keep_alive = true;
  int keep_alive_timeout_s = -1;
  bool gzip = false;
  bool deflate = false;
  std::string etag;
  std::string last_modified;
  std::string location;
  std::string content_type;
  const char* error = nullptr;

  // Raw (possibly compressed) body. Valid after Done until `in` changes.
  std::string_view body() const;
  std::size_t header_bytes() const { return body_start_; }

 private:
  enum class Phase : uint8_t { Headers, Length, Chunked, UntilClose, Done };
  enum class ChunkState : uint8_t { Size, Data, DataCrlf, Trailer };

  bool parse_headers(std::string_view head);
  Result parse_chunked(const Buffer& in);

  Phase phase_ = Phase::Headers;
  bool head_ = false;
  std::size_t scan_ = 0;
  std::size_t body_start_ = 0;
  int64_t content_length_ = -1;
  ChunkState cs_ = ChunkState::Size;
  std::size_t cpos_ = 0;
  std::size_t chunk_left_ = 0;
  Buffer dechunked_;
  bool use_dechunked_ = false;
  const char* in_data_ = nullptr;
  std::size_t body_len_ = 0;
};

// Reusable zlib inflater (gzip, zlib or raw deflate). Keeps its z_stream
// between responses, so there is no per-response init/alloc.
class Inflater {
 public:
  Inflater();
  ~Inflater();
  Inflater(const Inflater&) = delete;
  Inflater& operator=(const Inflater&) = delete;
  // Decompresses `in` into `out` (cleared first). Returns false on corrupt data.
  bool inflate(std::string_view in, Buffer& out);

 private:
  bool run(std::string_view in, Buffer& out, int window_bits);
  z_stream* zs_ = nullptr;
  int current_bits_ = 0;
};

}  // namespace stok::net
