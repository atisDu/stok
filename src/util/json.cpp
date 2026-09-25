#include "util/json.hpp"

#include <charconv>
#include <cstring>

#include "util/text.hpp"

namespace stok {

bool JsonDoc::fail(const char* msg, std::size_t at) {
  if (error_.empty()) error_ = std::string(msg) + " at offset " + std::to_string(at);
  return false;
}

void JsonDoc::skip_ws(std::size_t& i) const {
  while (i < src_.size()) {
    const char c = src_[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ++i;
    else break;
  }
}

bool JsonDoc::parse(std::string_view text) {
  src_ = text;
  nodes_.clear();
  error_.clear();
  nodes_.reserve(text.size() / 8 + 16);
  std::size_t i = 0;
  // Skip UTF-8 BOM.
  if (src_.size() >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF) i = 3;
  skip_ws(i);
  const uint32_t r = parse_value(i, 0);
  if (r == kNone) {
    if (nodes_.empty()) nodes_.push_back(Node{});
    return false;
  }
  skip_ws(i);
  if (i != src_.size()) return fail("trailing characters", i);
  return true;
}

static std::size_t scan_string_end(std::string_view s, std::size_t i) {
  // i points just past the opening quote. Returns index of the closing quote.
  while (i < s.size()) {
    const void* q = std::memchr(s.data() + i, '"', s.size() - i);
    if (!q) return std::string_view::npos;
    std::size_t k = static_cast<std::size_t>(static_cast<const char*>(q) - s.data());
    // Count preceding backslashes.
    std::size_t bs = 0;
    while (k - bs > i && s[k - bs - 1] == '\\') ++bs;
    if ((bs & 1) == 0) return k;
    i = k + 1;
  }
  return std::string_view::npos;
}

uint32_t JsonDoc::parse_value(std::size_t& i, int depth) {
  if (depth > 256) {
    fail("nesting too deep", i);
    return kNone;
  }
  skip_ws(i);
  if (i >= src_.size()) {
    fail("unexpected end", i);
    return kNone;
  }
  const uint32_t idx = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back(Node{});
  const char c = src_[i];
  if (c == '{' || c == '[') {
    const bool is_obj = c == '{';
    nodes_[idx].type = is_obj ? Type::Object : Type::Array;
    ++i;
    skip_ws(i);
    const char close = is_obj ? '}' : ']';
    if (i < src_.size() && src_[i] == close) {
      ++i;
      return idx;
    }
    uint32_t prev = kNone;
    for (;;) {
      std::string_view key;
      if (is_obj) {
        skip_ws(i);
        if (i >= src_.size() || src_[i] != '"') {
          fail("expected key", i);
          return kNone;
        }
        const std::size_t e = scan_string_end(src_, i + 1);
        if (e == std::string_view::npos) {
          fail("unterminated key", i);
          return kNone;
        }
        key = src_.substr(i + 1, e - i - 1);
        i = e + 1;
        skip_ws(i);
        if (i >= src_.size() || src_[i] != ':') {
          fail("expected ':'", i);
          return kNone;
        }
        ++i;
      }
      const uint32_t child = parse_value(i, depth + 1);
      if (child == kNone) return kNone;
      nodes_[child].key = key;
      if (prev == kNone) nodes_[idx].first_child = child;
      else nodes_[prev].next_sibling = child;
      prev = child;
      ++nodes_[idx].count;
      skip_ws(i);
      if (i >= src_.size()) {
        fail("unexpected end in container", i);
        return kNone;
      }
      if (src_[i] == ',') {
        ++i;
        continue;
      }
      if (src_[i] == close) {
        ++i;
        return idx;
      }
      fail("expected ',' or close", i);
      return kNone;
    }
  }
  if (c == '"') {
    const std::size_t e = scan_string_end(src_, i + 1);
    if (e == std::string_view::npos) {
      fail("unterminated string", i);
      return kNone;
    }
    nodes_[idx].type = Type::String;
    nodes_[idx].raw = src_.substr(i + 1, e - i - 1);
    i = e + 1;
    return idx;
  }
  if (c == 't' && src_.compare(i, 4, "true") == 0) {
    nodes_[idx].type = Type::Bool;
    nodes_[idx].boolean = true;
    i += 4;
    return idx;
  }
  if (c == 'f' && src_.compare(i, 5, "false") == 0) {
    nodes_[idx].type = Type::Bool;
    i += 5;
    return idx;
  }
  if (c == 'n' && src_.compare(i, 4, "null") == 0) {
    i += 4;
    return idx;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    const std::size_t b = i;
    ++i;
    while (i < src_.size()) {
      const char d = src_[i];
      if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') ++i;
      else break;
    }
    nodes_[idx].type = Type::Number;
    nodes_[idx].raw = src_.substr(b, i - b);
    double v = 0;
    auto [p, ec] = std::from_chars(src_.data() + b, src_.data() + i, v);
    if (ec != std::errc{}) {
      fail("bad number", b);
      return kNone;
    }
    nodes_[idx].number = v;
    return idx;
  }
  fail("unexpected character", i);
  return kNone;
}

const JsonDoc::Node* JsonDoc::get(const Node& obj, std::string_view key) const {
  if (obj.type != Type::Object) return nullptr;
  for (uint32_t c = obj.first_child; c != kNone; c = nodes_[c].next_sibling)
    if (nodes_[c].key == key) return &nodes_[c];
  return nullptr;
}

const JsonDoc::Node* JsonDoc::at(const Node& arr, std::size_t idx) const {
  if (arr.type != Type::Array && arr.type != Type::Object) return nullptr;
  std::size_t k = 0;
  for (uint32_t c = arr.first_child; c != kNone; c = nodes_[c].next_sibling, ++k)
    if (k == idx) return &nodes_[c];
  return nullptr;
}

std::string JsonDoc::unescape(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c != '\\' || i + 1 >= raw.size()) {
      out.push_back(c);
      continue;
    }
    const char e = raw[++i];
    switch (e) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        auto hex4 = [&](std::size_t at, uint32_t& v) {
          if (at + 4 > raw.size()) return false;
          v = 0;
          for (std::size_t k = at; k < at + 4; ++k) {
            const char h = raw[k];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
            else return false;
          }
          return true;
        };
        uint32_t cp = 0;
        if (!hex4(i + 1, cp)) {
          out.push_back('?');
          break;
        }
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < raw.size() && raw.compare(i + 1, 2, "\\u") == 0) {
          uint32_t lo = 0;
          if (hex4(i + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        text::append_utf8(out, cp);
        break;
      }
      default: out.push_back(e); break;  // \" \\ \/
    }
  }
  return out;
}

std::string JsonDoc::str(const Node* n) {
  if (!n || n->type != Type::String) return {};
  if (n->raw.find('\\') == std::string_view::npos) return std::string(n->raw);
  return unescape(n->raw);
}

double JsonDoc::num(const Node* n, double def) {
  if (!n) return def;
  if (n->type == Type::Number) return n->number;
  if (n->type == Type::String) {
    double v = 0;
    auto [p, ec] = std::from_chars(n->raw.data(), n->raw.data() + n->raw.size(), v);
    if (ec == std::errc{}) return v;
  }
  return def;
}

}  // namespace stok
