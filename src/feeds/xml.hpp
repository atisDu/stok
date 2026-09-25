#pragma once

#include <cstring>
#include <string_view>

#include "core/ascii.hpp"
#include "core/common.hpp"

// Zero-copy scanning for RSS/Atom. We only need a handful of fields per item,
// so instead of building a DOM we jump between tags with memmem
// (SIMD-accelerated in glibc) and return views into the response buffer.
// Enough for the well-formed, machine-generated feeds we poll. It doesn't
// resolve namespaces: tags match by literal prefix, e.g. "ndaq:IssueSymbol".
namespace stok::xml {

inline constexpr std::size_t npos = std::string_view::npos;

inline std::size_t find(std::string_view hay, std::string_view needle, std::size_t from = 0) noexcept {
  if (from > hay.size() || needle.size() > hay.size() - from) return npos;
  const void* p = memmem(hay.data() + from, hay.size() - from, needle.data(), needle.size());
  return p ? static_cast<std::size_t>(static_cast<const char*>(p) - hay.data()) : npos;
}

// True if the character after a tag name ends the name.
STOK_ALWAYS_INLINE bool name_end(char c) noexcept { return c == '>' || c == '/' || is_space(c); }

// Finds the next opening tag `<name` (whole name) at or after `from`.
// Searches for the (rarer) name bytes first and then checks the '<'.
inline std::size_t find_open(std::string_view doc, std::string_view name, std::size_t from = 0) noexcept {
  std::size_t pos = from + 1;
  for (;;) {
    const std::size_t p = find(doc, name, pos);
    if (p == npos) return npos;
    const std::size_t after = p + name.size();
    if (p >= 1 && doc[p - 1] == '<' && after < doc.size() && name_end(doc[after]) && p - 1 >= from) return p - 1;
    pos = p + 1;
  }
}

// Finds the next closing tag `</name>` at or after `from`.
inline std::size_t find_close(std::string_view doc, std::string_view name, std::size_t from) noexcept {
  std::size_t pos = from + 2;
  for (;;) {
    const std::size_t p = find(doc, name, pos);
    if (p == npos) return npos;
    const std::size_t after = p + name.size();
    if (p >= 2 && doc[p - 2] == '<' && doc[p - 1] == '/' && after < doc.size() &&
        (doc[after] == '>' || is_space(doc[after])))
      return p - 2;
    pos = p + 1;
  }
}

struct Element {
  std::string_view attrs;  // text between the tag name and '>' (without a trailing '/')
  std::string_view inner;  // raw content between open and close tags ("" if self-closing)
  std::size_t end = npos;  // index just past the element
  bool found() const { return end != npos; }
};

// Finds the next <name ...>...</name> (or <name .../>) at or after `from`.
// A close tag that appears inside a CDATA section is ignored.
inline Element next_element(std::string_view doc, std::string_view name, std::size_t from = 0) noexcept {
  Element e;
  const std::size_t lt = find_open(doc, name, from);
  if (lt == npos) return e;
  const std::size_t gt = doc.find('>', lt);
  if (gt == npos) return e;
  const std::size_t attr_b = lt + 1 + name.size();
  e.attrs = doc.substr(attr_b, gt - attr_b);
  if (doc[gt - 1] == '/') {
    e.attrs = e.attrs.substr(0, e.attrs.empty() ? 0 : e.attrs.size() - 1);
    e.end = gt + 1;
    return e;
  }
  // Look for CDATA only inside [pos, close): bounded by this element, so a
  // document without CDATA is never rescanned (linear, not quadratic).
  std::size_t pos = gt + 1;
  for (;;) {
    const std::size_t close = find_close(doc, name, pos);
    if (close == npos) return e;
    const std::size_t cdata = find(doc.substr(0, close), "<![CDATA[", pos);
    if (cdata != npos) {
      const std::size_t ce = find(doc, "]]>", cdata + 9);
      if (ce == npos) return e;
      pos = ce + 3;  // skip the section; re-find the close tag after it
      continue;
    }
    e.inner = doc.substr(gt + 1, close - gt - 1);
    const std::size_t te = doc.find('>', close);
    e.end = te == npos ? doc.size() : te + 1;
    return e;
  }
}

// Iterates top-level items (<item> for RSS, <entry> for Atom).
template <typename F>
inline void for_each_item(std::string_view doc, std::string_view tag, F&& f) {
  std::size_t pos = 0;
  for (;;) {
    Element e = next_element(doc, tag, pos);
    if (!e.found()) return;
    f(e.inner, e.attrs);
    pos = e.end;
  }
}

// Iterates all <name> elements in `xml`.
template <typename F>
inline void for_each_element(std::string_view xml, std::string_view name, F&& f) {
  std::size_t pos = 0;
  for (;;) {
    Element e = next_element(xml, name, pos);
    if (!e.found()) return;
    f(e);
    pos = e.end;
  }
}

// Inner text of the first <name> in `xml` (raw: entities not decoded).
inline std::string_view child_text(std::string_view xml, std::string_view name) noexcept {
  Element e = next_element(xml, name);
  return e.found() ? e.inner : std::string_view{};
}

// Value of attribute `attr` in an attribute string (quotes stripped).
inline std::string_view attr_value(std::string_view attrs, std::string_view attr) noexcept {
  std::size_t pos = 0;
  while (pos < attrs.size()) {
    const std::size_t p = find(attrs, attr, pos);
    if (p == npos) return {};
    const bool left_ok = p == 0 || is_space(attrs[p - 1]);
    std::size_t k = p + attr.size();
    while (k < attrs.size() && is_space(attrs[k])) ++k;
    if (left_ok && k < attrs.size() && attrs[k] == '=') {
      ++k;
      while (k < attrs.size() && is_space(attrs[k])) ++k;
      if (k < attrs.size() && (attrs[k] == '"' || attrs[k] == '\'')) {
        const char q = attrs[k];
        const std::size_t e = attrs.find(q, k + 1);
        if (e == npos) return {};
        return attrs.substr(k + 1, e - k - 1);
      }
    }
    pos = p + 1;
  }
  return {};
}

// If `raw` is entirely one CDATA section, returns its payload and sets
// *is_cdata.
inline std::string_view unwrap_cdata(std::string_view raw, bool* is_cdata = nullptr) noexcept {
  std::string_view t = trim(raw);
  if (t.size() >= 12 && t.compare(0, 9, "<![CDATA[") == 0 && t.compare(t.size() - 3, 3, "]]>") == 0 &&
      t.find("]]>", 9) == t.size() - 3) {
    if (is_cdata) *is_cdata = true;
    return t.substr(9, t.size() - 12);
  }
  if (is_cdata) *is_cdata = false;
  return raw;
}

}  // namespace stok::xml
