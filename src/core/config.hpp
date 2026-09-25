#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stok {

// Minimal INI-style config:
//
//   [section]            -> name="section", arg=""
//   [feed edgar_8k]      -> name="feed",    arg="edgar_8k"
//   key = value          # comment (a '#' or ';' preceded by whitespace)
//
// Sections with an arg can repeat (one per feed/host).
class Config {
 public:
  struct Section {
    std::string name;
    std::string arg;
    std::map<std::string, std::string> kv;

    std::optional<std::string> get(const std::string& key) const;
    std::string get_str(const std::string& key, const std::string& def = "") const;
    int64_t get_int(const std::string& key, int64_t def) const;
    double get_double(const std::string& key, double def) const;
    bool get_bool(const std::string& key, bool def) const;
    std::vector<std::string> get_list(const std::string& key) const;  // comma-separated
  };

  static Config parse(std::string_view text, std::string* error = nullptr);
  static std::optional<Config> load_file(const std::string& path, std::string* error = nullptr);

  // First section with this name (and empty arg); returns an empty section if
  // absent so getters fall back to defaults.
  const Section& section(const std::string& name) const;
  std::vector<const Section*> sections(const std::string& name) const;

  std::string str(const std::string& sec, const std::string& key, const std::string& def = "") const {
    return section(sec).get_str(key, def);
  }
  int64_t integer(const std::string& sec, const std::string& key, int64_t def) const {
    return section(sec).get_int(key, def);
  }
  double dbl(const std::string& sec, const std::string& key, double def) const {
    return section(sec).get_double(key, def);
  }
  bool boolean(const std::string& sec, const std::string& key, bool def) const {
    return section(sec).get_bool(key, def);
  }

  // For tests and command-line overrides.
  void set(const std::string& sec, const std::string& key, const std::string& value);

 private:
  std::vector<Section> sections_;
  Section empty_;
};

}  // namespace stok
