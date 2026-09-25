#include "market/baseline.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "core/ascii.hpp"
#include "util/file.hpp"

namespace stok {

std::string BaselineStore::baseline_path() const { return fileutil::join(dir_, "baseline.tsv"); }
std::string BaselineStore::history_path() const { return fileutil::join(dir_, "history.tsv"); }

std::size_t BaselineStore::append_session(const std::string& date, const MarketBoard& board,
                                          const SymbolTable& symbols) {
  fileutil::make_dirs(dir_);
  std::string rows;
  std::size_t n = 0;
  char line[128];
  for (uint32_t i = 0; i < board.size() && i < symbols.size(); ++i) {
    const MarketHot& h = board.writer_view(i).hot;
    if (h.day_volume == 0) continue;
    const int32_t close = h.close_px ? h.close_px : h.rth_last_px ? h.rth_last_px : h.last_px;
    std::snprintf(line, sizeof(line), "%s\t%s\t%llu\t%.4f\n", date.c_str(), symbols[i].ticker.c_str(),
                  static_cast<unsigned long long>(h.day_volume), close / 1e4);
    rows += line;
    ++n;
  }
  if (n == 0) return 0;
  // Drop any earlier rows for the same date (re-runs), then append.
  std::string existing = fileutil::read_file(history_path()).value_or("");
  std::string kept;
  kept.reserve(existing.size() + rows.size());
  for_each_line(existing, [&](std::string_view l) {
    if (l.empty() || l.substr(0, date.size()) == date) return;
    kept.append(l);
    kept.push_back('\n');
  });
  kept += rows;
  fileutil::write_file_atomic(history_path(), kept);
  return n;
}

bool BaselineStore::rebuild(int days, std::string* report) {
  auto hist = fileutil::read_file(history_path());
  if (!hist) return false;
  std::set<std::string> dates;
  for_each_line(*hist, [&](std::string_view l) {
    const auto tab = l.find('\t');
    if (tab != std::string_view::npos) dates.insert(std::string(l.substr(0, tab)));
  });
  std::vector<std::string> recent(dates.begin(), dates.end());
  if (static_cast<int>(recent.size()) > days) recent.erase(recent.begin(), recent.end() - days);
  const std::set<std::string> keep(recent.begin(), recent.end());
  struct Acc {
    double vol_sum = 0;
    uint32_t n = 0;
    std::string last_date;
    double last_close = 0;
  };
  std::map<std::string, Acc> acc;
  std::string trimmed;
  for_each_line(*hist, [&](std::string_view l) {
    std::string_view f[4];
    int n = 0;
    split(l, '\t', [&](std::string_view c) {
      if (n < 4) f[n++] = c;
    });
    if (n < 4 || !keep.count(std::string(f[0]))) return;
    trimmed.append(l);
    trimmed.push_back('\n');
    Acc& a = acc[std::string(f[1])];
    a.vol_sum += parse_double(f[2]).value_or(0);
    ++a.n;
    if (std::string(f[0]) >= a.last_date) {
      a.last_date = std::string(f[0]);
      a.last_close = parse_double(f[3]).value_or(0);
    }
  });
  std::string out = "ticker\tprev_close\tadv\tdays\tasof\n";
  char line[160];
  for (const auto& [t, a] : acc) {
    std::snprintf(line, sizeof(line), "%s\t%.4f\t%.0f\t%u\t%s\n", t.c_str(), a.last_close,
                  a.n ? a.vol_sum / a.n : 0.0, a.n, a.last_date.c_str());
    out += line;
  }
  fileutil::write_file_atomic(history_path(), trimmed);
  const bool ok = fileutil::write_file_atomic(baseline_path(), out);
  if (report) *report = std::to_string(acc.size()) + " symbols over " + std::to_string(recent.size()) + " sessions";
  return ok;
}

}  // namespace stok
