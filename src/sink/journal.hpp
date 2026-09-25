#pragma once

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/spsc_queue.hpp"
#include "core/waker.hpp"
#include "engine/scorer.hpp"
#include "engine/signals.hpp"
#include "ref/symbols.hpp"

namespace stok {

// Append-only JSONL journal, one directory per US-Eastern trading day:
//   <dir>/YYYY-MM-DD/news.jsonl      every item seen, with score and context
//   <dir>/YYYY-MM-DD/signals.jsonl   every signal emitted
//   <dir>/YYYY-MM-DD/outcomes.jsonl  price/volume at +1m/+5m/+15m/+30m/+60m after each watched story
//   <dir>/YYYY-MM-DD/stats.jsonl     periodic latency/feed stats
// This is the dataset for tuning rules and deciding whether an edge exists
// (DuckDB/pandas read JSONL directly). Formatting and I/O happen on this
// thread, never on the engine's.
class Journal {
 public:
  Journal(std::string dir, const SymbolTable& symbols, const Scorer* scorer, std::vector<std::string> source_names);
  ~Journal();

  void run(SpscQueue<JournalRecord>& q, Waker& waker, const std::atomic<bool>& stop);
  void write(const JournalRecord& r);
  void flush();

  // Formatting (exposed for tests).
  std::string news_json(const JournalRecord& r) const;
  std::string signal_json(const Signal& s) const;
  std::string outcome_json(const Outcome& o) const;

 private:
  FILE* file_for(const std::string& name, uint64_t t_ns);
  const char* source_name(uint16_t s) const;

  std::string dir_;
  const SymbolTable& symbols_;
  const Scorer* scorer_;
  std::vector<std::string> sources_;
  std::string day_;
  std::map<std::string, FILE*> files_;
};

}  // namespace stok
