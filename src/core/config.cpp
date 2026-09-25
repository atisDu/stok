#include "core/config.hpp"

#include <fstream>
#include <sstream>

#include "core/ascii.hpp"

namespace stok {

std::optional<std::string> Config::Section::get(const std::string& key) const {
  auto it = kv.find(key);
  if (it == kv.end()) return std::nullopt;
  return it->second;
}

std::string Config::Section::get_str(const std::string& key, const std::string& def) const {
  auto v = get(key);
  return v ? *v : def;
}

int64_t Config::Section::get_int(const std::string& key, int64_t def) const {
  auto v = get(key);
  if (!v || v->empty()) return def;
  if (auto i = parse_int<int64_t>(*v)) return *i;
  if (auto d = parse_double(*v)) return static_cast<int64_t>(*d);  // allows "20e6"
  return def;
}

double Config::Section::get_double(const std::string& key, double def) const {
  auto v = get(key);
  if (!v || v->empty()) return def;
  if (auto d = parse_double(*v)) return *d;
  return def;
}

bool Config::Section::get_bool(const std::string& key, bool def) const {
  auto v = get(key);
  if (!v || v->empty()) return def;
  const std::string s = to_lower_copy(*v);
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  return def;
}

std::vector<std::string> Config::Section::get_list(const std::string& key) const {
  std::vector<std::string> out;
  auto v = get(key);
  if (!v) return out;
  split(*v, ',', [&](std::string_view part) {
    part = trim(part);
    if (!part.empty()) out.emplace_back(part);
  });
  return out;
}

static std::string_view strip_comment(std::string_view line) {
  if (!line.empty() && (line[0] == '#' || line[0] == ';')) return {};
  for (std::size_t i = 1; i < line.size(); ++i) {
    if ((line[i] == '#' || line[i] == ';') && is_space(line[i - 1])) return line.substr(0, i);
  }
  return line;
}

Config Config::parse(std::string_view text, std::string* error) {
  Config cfg;
  Section* cur = nullptr;
  int lineno = 0;
  for_each_line(text, [&](std::string_view raw) {
    ++lineno;
    std::string_view line = trim(strip_comment(trim(raw)));
    if (line.empty()) return;
    if (line.front() == '[') {
      if (line.back() != ']') {
        if (error && error->empty()) *error = "line " + std::to_string(lineno) + ": unterminated section header";
        return;
      }
      std::string_view inner = trim(line.substr(1, line.size() - 2));
      Section s;
      const std::size_t sp = inner.find_first_of(" \t");
      if (sp == std::string_view::npos) {
        s.name = std::string(inner);
      } else {
        s.name = std::string(inner.substr(0, sp));
        s.arg = std::string(trim(inner.substr(sp + 1)));
      }
      cfg.sections_.push_back(std::move(s));
      cur = &cfg.sections_.back();
      return;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      if (error && error->empty()) *error = "line " + std::to_string(lineno) + ": expected key = value";
      return;
    }
    if (!cur) {
      cfg.sections_.push_back(Section{"general", "", {}});
      cur = &cfg.sections_.back();
    }
    std::string key(trim(line.substr(0, eq)));
    std::string val(trim(line.substr(eq + 1)));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    cur->kv[key] = val;
  });
  return cfg;
}

std::optional<Config> Config::load_file(const std::string& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string err;
  Config c = parse(ss.str(), &err);
  if (!err.empty()) {
    if (error) *error = path + ": " + err;
    return std::nullopt;
  }
  return c;
}

const Config::Section& Config::section(const std::string& name) const {
  for (const auto& s : sections_)
    if (s.name == name && s.arg.empty()) return s;
  return empty_;
}

std::vector<const Config::Section*> Config::sections(const std::string& name) const {
  std::vector<const Section*> out;
  for (const auto& s : sections_)
    if (s.name == name) out.push_back(&s);
  return out;
}

void Config::set(const std::string& sec, const std::string& key, const std::string& value) {
  for (auto& s : sections_) {
    if (s.name == sec && s.arg.empty()) {
      s.kv[key] = value;
      return;
    }
  }
  sections_.push_back(Section{sec, "", {{key, value}}});
}

}  // namespace stok
