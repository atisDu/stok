#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stok {

// Small zero-copy JSON DOM for reference files (SEC ticker maps, XBRL frames).
// Nodes live in one vector; strings are views into the source text, which must
// outlive the document. Call unescape() for strings that may contain escapes.
class JsonDoc {
 public:
  enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
  static constexpr uint32_t kNone = UINT32_MAX;

  struct Node {
    Type type = Type::Null;
    bool boolean = false;
    uint32_t first_child = kNone;
    uint32_t next_sibling = kNone;
    uint32_t count = 0;        // children
    std::string_view key;      // raw key (object members only)
    std::string_view raw;      // raw string contents (no quotes) or number text
    double number = 0.0;
  };

  bool parse(std::string_view text);
  const std::string& error() const { return error_; }

  const Node& root() const { return nodes_[0]; }
  const Node& node(uint32_t idx) const { return nodes_[idx]; }

  // Object member lookup (linear; objects in these files are small).
  const Node* get(const Node& obj, std::string_view key) const;
  const Node* at(const Node& arr, std::size_t idx) const;

  template <typename F>
  void for_each(const Node& parent, F&& f) const {
    for (uint32_t c = parent.first_child; c != kNone; c = nodes_[c].next_sibling) f(nodes_[c]);
  }

  static std::string unescape(std::string_view raw);
  // Convenience: string value (unescaped) or empty.
  static std::string str(const Node* n);
  static double num(const Node* n, double def = 0.0);

 private:
  uint32_t parse_value(std::size_t& i, int depth);
  bool fail(const char* msg, std::size_t at);
  void skip_ws(std::size_t& i) const;

  std::string_view src_;
  std::vector<Node> nodes_;
  std::string error_;
};

}  // namespace stok
