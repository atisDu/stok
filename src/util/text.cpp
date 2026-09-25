#include "util/text.hpp"

#include <cstring>

#include "core/ascii.hpp"
#include "core/hash.hpp"

namespace stok::text {

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x110000) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

namespace {

struct Named {
  const char* name;
  uint32_t cp;
};

// The entities that actually show up in wire copy and SEC pages.
constexpr Named kNamed[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xA0},    {"rsquo", 0x2019},  {"lsquo", 0x2018},  {"rdquo", 0x201D},  {"ldquo", 0x201C},
    {"ndash", 0x2013}, {"mdash", 0x2014},  {"hellip", 0x2026}, {"reg", 0xAE},      {"trade", 0x2122},
    {"copy", 0xA9},    {"bull", 0x2022},   {"middot", 0xB7},   {"deg", 0xB0},      {"eacute", 0xE9},
    {"euro", 0x20AC},  {"pound", 0xA3},    {"cent", 0xA2},     {"sect", 0xA7},     {"para", 0xB6},
    {"times", 0xD7},   {"frac12", 0xBD},   {"shy", 0xAD},      {"ensp", 0x2002},   {"emsp", 0x2003},
    {"thinsp", 0x2009}, {"zwnj", 0x200C},  {"zwj", 0x200D},    {"iexcl", 0xA1},    {"laquo", 0xAB},
    {"raquo", 0xBB},
};

// Tries to decode one entity starting at in[i] == '&'. On success appends and
// returns the number of bytes consumed, else 0.
std::size_t decode_one(std::string_view in, std::size_t i, std::string& out) {
  const std::size_t semi_limit = std::min(in.size(), i + 12);
  std::size_t semi = i + 1;
  while (semi < semi_limit && in[semi] != ';' && in[semi] != '&' && !is_space(in[semi])) ++semi;
  if (semi >= semi_limit || in[semi] != ';') return 0;
  std::string_view body = in.substr(i + 1, semi - i - 1);
  if (body.empty()) return 0;
  if (body[0] == '#') {
    uint32_t cp = 0;
    bool ok = body.size() > 1;
    if (body.size() > 1 && (body[1] == 'x' || body[1] == 'X')) {
      ok = body.size() > 2;
      for (std::size_t k = 2; k < body.size() && ok; ++k) {
        const char c = body[k];
        cp <<= 4;
        if (is_digit(c)) cp |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
        else ok = false;
      }
    } else {
      for (std::size_t k = 1; k < body.size() && ok; ++k) {
        if (!is_digit(body[k])) ok = false;
        else cp = cp * 10 + static_cast<uint32_t>(body[k] - '0');
      }
    }
    if (!ok || cp == 0 || cp > 0x10FFFF) return 0;
    append_utf8(out, cp);
    return semi - i + 1;
  }
  for (const auto& n : kNamed) {
    if (body == n.name) {
      append_utf8(out, n.cp);
      return semi - i + 1;
    }
  }
  return 0;
}

bool is_block_tag(std::string_view name) {
  static const char* kBlock[] = {"p",  "br", "div", "li", "tr", "td", "th", "h1", "h2", "h3", "h4",
                                 "h5", "h6", "ul",  "ol", "table", "section", "article", "blockquote",
                                 "hr", "pre", "title", "dd", "dt"};
  for (const char* b : kBlock)
    if (iequals(name, b)) return true;
  return false;
}

}  // namespace

void append_decoded_entities(std::string_view in, std::string& out) {
  std::size_t i = 0;
  while (i < in.size()) {
    const void* amp = std::memchr(in.data() + i, '&', in.size() - i);
    if (!amp) {
      out.append(in.data() + i, in.size() - i);
      return;
    }
    const std::size_t a = static_cast<std::size_t>(static_cast<const char*>(amp) - in.data());
    out.append(in.data() + i, a - i);
    const std::size_t used = decode_one(in, a, out);
    if (used) {
      i = a + used;
    } else {
      out.push_back('&');
      i = a + 1;
    }
  }
}

void html_to_text(std::string_view html, std::string& out, std::size_t max_out, char block_sep) {
  const std::size_t start_len = out.size();
  bool last_space = out.empty() || is_space(out.back());
  auto push_space = [&] {
    if (!last_space) {
      out.push_back(' ');
      last_space = true;
    }
  };
  auto push_block = [&] {
    if (block_sep == ' ') {
      push_space();
      return;
    }
    while (out.size() > start_len && out.back() == ' ') out.pop_back();
    if (out.size() > start_len && out.back() != block_sep) out.push_back(block_sep);
    last_space = true;
  };
  std::size_t i = 0;
  const std::size_t n = html.size();
  while (i < n && out.size() - start_len < max_out) {
    const char c = html[i];
    if (c == '<') {
      // Comment
      if (html.compare(i, 4, "<!--") == 0) {
        const std::size_t e = html.find("-->", i + 4);
        i = e == std::string_view::npos ? n : e + 3;
        continue;
      }
      // CDATA: copy content verbatim.
      if (html.compare(i, 9, "<![CDATA[") == 0) {
        const std::size_t e = html.find("]]>", i + 9);
        const std::size_t stop = e == std::string_view::npos ? n : e;
        html_to_text(html.substr(i + 9, stop - i - 9), out, max_out - (out.size() - start_len), block_sep);
        last_space = out.empty() || is_space(out.back());
        i = e == std::string_view::npos ? n : e + 3;
        continue;
      }
      std::size_t j = i + 1;
      if (j < n && html[j] == '/') ++j;
      const std::size_t name_b = j;
      while (j < n && (is_alnum(html[j]) || html[j] == ':' || html[j] == '-')) ++j;
      const std::string_view name = html.substr(name_b, j - name_b);
      if (name.empty()) {
        // Not a tag ("a < b"): keep the character.
        out.push_back(c);
        last_space = false;
        ++i;
        continue;
      }
      const std::size_t gt = html.find('>', j);
      if (gt == std::string_view::npos) break;
      if (html[i + 1] != '/' && (iequals(name, "script") || iequals(name, "style"))) {
        const std::size_t close = ifind(html, iequals(name, "script") ? "</script" : "</style", gt);
        if (close == std::string_view::npos) break;
        const std::size_t close_gt = html.find('>', close);
        i = close_gt == std::string_view::npos ? n : close_gt + 1;
        push_block();
        continue;
      }
      if (is_block_tag(name)) push_block();
      i = gt + 1;
      continue;
    }
    if (c == '&') {
      std::string tmp;
      const std::size_t used = decode_one(html, i, tmp);
      if (used) {
        // Non-breaking and other Unicode spaces act as whitespace.
        if (tmp == "\xC2\xA0" || tmp == "\xE2\x80\x82" || tmp == "\xE2\x80\x83" || tmp == "\xE2\x80\x89") {
          push_space();
        } else if (tmp == "\xC2\xAD" || tmp == "\xE2\x80\x8C" || tmp == "\xE2\x80\x8D") {
          // soft hyphen / zero-width joiners: drop
        } else {
          out += tmp;
          last_space = false;
        }
        i += used;
        continue;
      }
    }
    if (is_space(c)) {
      push_space();
    } else {
      out.push_back(c);
      last_space = false;
    }
    ++i;
  }
  // Trim trailing separators we added.
  while (out.size() > start_len && (out.back() == ' ' || out.back() == block_sep)) out.pop_back();
  if (out.size() - start_len > max_out) {
    std::size_t cut = start_len + max_out;
    while (cut > start_len && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
}

void collapse_whitespace(std::string& s) {
  std::size_t w = 0;
  bool sp = true;
  for (std::size_t r = 0; r < s.size(); ++r) {
    const char c = s[r];
    if (is_space(c)) {
      if (!sp) {
        s[w++] = ' ';
        sp = true;
      }
    } else {
      s[w++] = c;
      sp = false;
    }
  }
  while (w > 0 && s[w - 1] == ' ') --w;
  s.resize(w);
}

uint64_t headline_key(std::string_view title) {
  char buf[512];
  std::size_t n = 0;
  bool sep = true;
  for (char c : title) {
    if (n >= sizeof(buf) - 1) break;
    if (is_alnum(c)) {
      buf[n++] = to_lower(c);
      sep = false;
    } else if (!sep) {
      buf[n++] = ' ';
      sep = true;
    }
  }
  while (n > 0 && buf[n - 1] == ' ') --n;
  return hash_bytes(buf, n);
}

void append_json_escaped(std::string& out, std::string_view s) {
  static const char* kHex = "0123456789abcdef";
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 15]);
        } else {
          out.push_back(ch);
        }
    }
  }
}

}  // namespace stok::text
