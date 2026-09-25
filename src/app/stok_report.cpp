// stok-report: reads the journal and answers "is there an edge yet?":
// which sources are first, what happened to prices after each kind of story,
// and how the paper trader did against the go/no-go criteria in PLAN.md.

#include <cstdio>
#include <string>

#include "app/settings.hpp"
#include "core/log.hpp"
#include "research/report.hpp"

using namespace stok;

int main(int argc, char** argv) {
  std::string dir, config_path, from, to;
  research::GoNoGo g;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-d" || a == "--dir") && i + 1 < argc) dir = argv[++i];
    else if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
    else if (a == "--from" && i + 1 < argc) from = argv[++i];
    else if (a == "--to" && i + 1 < argc) to = argv[++i];
    else if (a == "--min-trades" && i + 1 < argc) g.min_trades = std::atoi(argv[++i]);
    else if (a == "--min-days" && i + 1 < argc) g.min_span_days = std::atoi(argv[++i]);
    else if (a == "--min-pf" && i + 1 < argc) g.min_profit_factor = std::atof(argv[++i]);
    else {
      std::fprintf(stderr,
                   "usage: stok-report [-c config | -d journal_dir] [--from YYYY-MM-DD] [--to YYYY-MM-DD]\n"
                   "                   [--min-trades 150] [--min-days 56] [--min-pf 1.3]\n");
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  if (dir.empty()) {
    Settings s;
    if (!config_path.empty()) {
      std::string err;
      auto cfg = Config::load_file(config_path, &err);
      if (!cfg || !load_settings(*cfg, s, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 1;
      }
    }
    dir = s.journal_dir.empty() ? "data/journal" : s.journal_dir;
  }
  const auto data = research::load_journal(dir, from, to);
  if (data.days.empty()) {
    std::fprintf(stderr, "no journal days found in %s\n", dir.c_str());
    return 1;
  }
  std::fputs(research::render_report(data, g).c_str(), stdout);
  Logger::instance().stop();
  return 0;
}
