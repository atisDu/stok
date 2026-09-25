#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace stok::fileutil {

std::optional<std::string> read_file(const std::string& path);
// Writes to path.tmp then renames, so readers never see a partial file.
bool write_file_atomic(const std::string& path, std::string_view content);
bool make_dirs(const std::string& path);
bool exists(const std::string& path);
std::string join(const std::string& a, const std::string& b);

}  // namespace stok::fileutil
