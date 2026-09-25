#include "util/file.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace stok::fileutil {

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_file_atomic(const std::string& path, std::string_view content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool make_dirs(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec || std::filesystem::is_directory(path);
}

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

}  // namespace stok::fileutil
