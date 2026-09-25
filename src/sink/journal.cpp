#include "sink/journal.hpp"

#include <cinttypes>
#include <cmath>
#include <cstring>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "ref/filings.hpp"
#include "util/file.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace stok {

namespace {

struct J {
  std::string s;
  bool first = true;
  J() { s.push_back('{'); }
  void key(const char* k) {
    if (!first) s.push_back(',');
    first = false;
    s.push_back('"');
    s += k;
    s += "\":";
  }
  void str(const char* k, std::string_view v) {
    key(k);
    s.push_back('"');
    text::append_json_escaped(s, v);
    s.push_back('"');
  }
  void num(const char* k, double v, int prec = 4) {
    key(k);
    if (!std::isfinite(v)) {
      s += "null";
      return;
    }
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    // trim trailing zeros
    std::string_view t(b);
    if (t.find('.') != std::string_view::npos) {
      while (!t.empty() && t.back() == '0') t.remove_suffix(1);
      if (!t.empty() && t.back() == '.') t.remove_suffix(1);
    }
    s += t;
  }
  void integer(const char* k, int64_t v) {
    key(k);
    s += std::to_string(v);
  }
  void uinteger(const char* k, uint64_t v) {
    key(k);
    s += std::to_string(v);
  }
  void boolean(const char* k, bool v) {
    key(k);
    s += v ? "true" : "false";
  }
  void time(const char* k, int64_t ns) {
    if (ns <= 0) return;
    str(k, timeutil::format_utc_iso(ns));
  }
  void hex(const char* k, uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016" PRIx64, v);
    str(k, b);
  }
  std::string done() {
    s.push_back('}');
    return std::move(s);
  }
};

}  // namespace

Journal::Journal(std::string dir, const SymbolTable& symbols, const Scorer* scorer,
                 std::vector<std::string> source_names)
    : dir_(std::move(dir)), symbols_(symbols), scorer_(scorer), sources_(std::move(source_names)) {}

Journal::~Journal() {
  for (auto& [n, f] : files_)
    if (f) std::fclose(f);
}

const char* Journal::source_name(uint16_t s) const {
  return s < sources_.size() ? sources_[s].c_str() : "?";
}

FILE* Journal::file_for(const std::string& name, uint64_t t_ns) {
  const std::string day = timeutil::eastern_date_str(static_cast<int64_t>(t_ns ? t_ns : wall_ns()));
  if (day != day_) {
    for (auto& [n, f] : files_)
      if (f) std::fclose(f);
    files_.clear();
    day_ = day;
    fileutil::make_dirs(fileutil::join(dir_, day_));
  }
  auto it = files_.find(name);
  if (it != files_.end()) return it->second;
  const std::string path = fileutil::join(fileutil::join(dir_, day_), name + ".jsonl");
  FILE* f = std::fopen(path.c_str(), "a");
  if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 18);
  else LOG_ERROR("journal: cannot open %s", path.c_str());
  files_[name] = f;
  return f;
}

std::string Journal::news_json(const JournalRecord& r) const {
  const NewsEvent& e = r.news;
  J j;
  j.time("t", static_cast<int64_t>(e.recv_ns));
  j.str("src", source_name(e.source));
  j.str("kind", event_kind_name(e.kind));
  j.hex("id", e.id_hash);
  j.uinteger("recv_ns", e.recv_ns);
  j.uinteger("sent_ns", e.sent_ns);
  j.integer("pub_ns", e.published_ns);
  j.uinteger("engine_ns", r.engine_ns);
  if (e.published_ns > 0 && e.recv_ns > static_cast<uint64_t>(e.published_ns))
    j.num("pub_to_recv_ms", static_cast<double>(e.recv_ns - static_cast<uint64_t>(e.published_ns)) / 1e6, 1);
  j.str("title", e.title.view());
  j.str("link", e.link.view());
  j.key("tickers");
  j.s.push_back('[');
  for (int i = 0; i < e.n_tickers; ++i) {
    if (i) j.s.push_back(',');
    j.s.push_back('"');
    if (e.tickers[i] < symbols_.size()) text::append_json_escaped(j.s, symbols_[e.tickers[i]].ticker);
    j.s.push_back('"');
  }
  j.s.push_back(']');
  if (e.flags & kEvNameMatched) j.boolean("name_matched", true);
  if (e.n_unresolved) {
    j.key("unresolved");
    j.s.push_back('[');
    for (int i = 0; i < e.n_unresolved; ++i) {
      if (i) j.s.push_back(',');
      j.s.push_back('"');
      text::append_json_escaped(j.s, e.unresolved[i]);
      j.s.push_back('"');
    }
    j.s.push_back(']');
  }
  if (e.kind == EventKind::Filing || e.kind == EventKind::FilingDoc) {
    j.str("form", e.form);
    j.uinteger("cik", e.cik);
    if (e.items_mask) j.str("items", form8k_items_str(e.items_mask));
  }
  if (e.kind == EventKind::Halt) {
    j.str("halt_reason", e.halt.reason);
    j.str("market", e.halt.market);
    j.time("halt_time", e.halt.halt_ns);
    j.time("resume_trade", e.halt.resume_trade_ns);
    j.integer("halt_ns", e.halt.halt_ns);
    j.integer("resume_quote_ns", e.halt.resume_quote_ns);
    j.integer("resume_trade_ns", e.halt.resume_trade_ns);
    if (e.halt.pause_threshold > 0) j.num("pause_threshold", e.halt.pause_threshold);
  } else {
    const ScoreResult& s = r.score;
    j.integer("score", s.score);
    j.integer("text_score", s.text_score);
    j.str("catalyst", catalyst_name(s.catalyst));
    if (s.flags) j.str("flags", score_flags_str(s.flags));
    if (s.amount_usd > 0) j.num("amount_usd", s.amount_usd, 0);
    if (s.materiality > 0) j.num("materiality", s.materiality, 4);
    if (scorer_ && s.n_matched) {
      j.key("matched");
      j.s.push_back('[');
      for (int i = 0; i < s.n_matched; ++i) {
        if (i) j.s.push_back(',');
        j.s.push_back('"');
        const auto& rule = scorer_->rule(s.matched[i]);
        text::append_json_escaped(j.s, rule.kind_str + ":" + rule.phrase);
        j.s.push_back('"');
      }
      j.s.push_back(']');
    }
  }
  if (r.news_sym < symbols_.size()) {
    j.str("sym", symbols_[r.news_sym].ticker);
    if (r.news_price > 0) j.num("price", r.news_price);
    if (r.news_market_cap > 0) j.num("mcap", r.news_market_cap, 0);
    if (symbols_[r.news_sym].shares_outstanding > 0) j.num("shares_out", symbols_[r.news_sym].shares_outstanding, 0);
    j.boolean("in_universe", r.news_in_universe);
    if (r.news_dilution) j.str("dilution", dilution_flags_str(r.news_dilution));
  }
  if (r.news_duplicate) {
    j.boolean("dup", true);
    j.str("first_src", source_name(r.first_source));
    j.num("behind_first_ms", static_cast<double>(r.first_seen_lag_ns) / 1e6, 1);
  }
  // The full body is kept so the journal can be replayed by stok-backtest.
  j.str("body", e.body.view());
  return j.done();
}

std::string Journal::signal_json(const Signal& s) const {
  J j;
  j.time("t", static_cast<int64_t>(s.t_signal_ns));
  j.str("tier", tier_name(s.tier));
  j.str("ticker", s.ticker);
  j.str("exchange", s.exchange);
  j.integer("score", s.score);
  j.str("catalyst", catalyst_name(s.catalyst));
  j.num("price", s.price);
  j.num("ref_price", s.ref_price);
  j.num("move_pct", s.move_pct, 2);
  j.num("rvol", s.rvol, 2);
  j.num("dollar_volume", s.dollar_volume, 0);
  j.num("vwap", s.vwap);
  if (s.spread_pct > 0) {
    j.num("bid", s.bid);
    j.num("ask", s.ask);
    j.num("spread_pct", s.spread_pct, 2);
  }
  if (s.market_cap > 0) j.num("mcap", s.market_cap, 0);
  if (s.shares_out > 0) j.num("shares_out", s.shares_out, 0);
  if (s.amount_usd > 0) j.num("amount_usd", s.amount_usd, 0);
  if (s.materiality > 0) j.num("materiality", s.materiality, 4);
  if (s.score_flags) j.str("flags", score_flags_str(s.score_flags));
  if (s.dilution_flags) j.str("dilution", dilution_flags_str(s.dilution_flags));
  if (s.halt_reason[0]) j.str("halt_reason", s.halt_reason);
  j.str("why", s.why.view());
  j.str("title", s.title.view());
  j.str("link", s.link.view());
  j.str("src", source_name(s.source));
  j.hex("news_id", s.news_id);
  j.uinteger("news_recv_ns", s.t_news_recv_ns);
  j.uinteger("signal_ns", s.t_signal_ns);
  if (s.t_news_recv_ns && s.t_signal_ns > s.t_news_recv_ns)
    j.num("news_to_signal_ms", static_cast<double>(s.t_signal_ns - s.t_news_recv_ns) / 1e6, 3);
  return j.done();
}

std::string Journal::outcome_json(const Outcome& o) const {
  J j;
  j.time("t", static_cast<int64_t>(o.t_ns));
  j.str("ticker", o.ticker);
  j.hex("news_id", o.news_id);
  j.uinteger("horizon_s", o.horizon_s);
  j.integer("score", o.score);
  j.str("catalyst", catalyst_name(o.catalyst));
  j.str("tier_reached", tier_name(o.tier_reached));
  j.num("ref_price", o.ref_price);
  j.num("price", o.price);
  j.num("move_pct", o.move_pct, 2);
  j.num("max_move_pct", o.max_move_pct, 2);
  j.num("min_move_pct", o.min_move_pct, 2);
  j.num("volume_since", o.volume_since, 0);
  j.num("dollar_volume_since", o.dollar_volume_since, 0);
  return j.done();
}

std::string Journal::trade_json(const PaperTrade& t) const {
  J j;
  j.time("t", static_cast<int64_t>(t.closed ? t.exit_ns : t.entry_ns));
  j.str("event", t.closed ? "close" : "open");
  j.str("ticker", t.ticker);
  j.str("tier", tier_name(t.tier));
  j.str("catalyst", catalyst_name(t.catalyst));
  j.integer("score", t.score);
  j.hex("news_id", t.news_id);
  j.uinteger("signal_ns", t.signal_ns);
  j.uinteger("entry_ns", t.entry_ns);
  j.num("signal_px", t.signal_px);
  j.num("entry_px", t.entry_px);
  j.num("shares", t.shares, 0);
  j.boolean("quote_entry", t.quote_entry);
  if (t.size_over_touch) j.boolean("size_over_touch", true);
  j.num("entry_delay_ms", t.entry_ns > t.signal_ns ? static_cast<double>(t.entry_ns - t.signal_ns) / 1e6 : 0.0, 1);
  if (t.closed) {
    j.uinteger("exit_ns", t.exit_ns);
    j.num("exit_px", t.exit_px);
    j.boolean("quote_exit", t.quote_exit);
    j.str("exit_reason", t.exit_reason);
    j.num("hold_s", static_cast<double>(t.exit_ns - t.entry_ns) / 1e9, 1);
    j.num("fees", t.fees, 2);
    j.num("pnl_usd", t.pnl_usd, 2);
    j.num("pnl_pct", t.pnl_pct, 3);
    j.num("mfe_pct", t.mfe_pct, 2);
    j.num("mae_pct", t.mae_pct, 2);
  }
  return j.done();
}

void Journal::write(const JournalRecord& r) {
  switch (r.type) {
    case JournalRecord::Type::News: {
      if (FILE* f = file_for("news", r.news.recv_ns)) {
        const std::string line = news_json(r);
        std::fwrite(line.data(), 1, line.size(), f);
        std::fputc('\n', f);
      }
      break;
    }
    case JournalRecord::Type::Signal: {
      if (FILE* f = file_for("signals", r.signal.t_signal_ns)) {
        const std::string line = signal_json(r.signal);
        std::fwrite(line.data(), 1, line.size(), f);
        std::fputc('\n', f);
      }
      break;
    }
    case JournalRecord::Type::Outcome: {
      if (FILE* f = file_for("outcomes", r.outcome.t_ns)) {
        const std::string line = outcome_json(r.outcome);
        std::fwrite(line.data(), 1, line.size(), f);
        std::fputc('\n', f);
      }
      break;
    }
    case JournalRecord::Type::Trade: {
      if (FILE* f = file_for("trades", r.trade.closed ? r.trade.exit_ns : r.trade.entry_ns)) {
        const std::string line = trade_json(r.trade);
        std::fwrite(line.data(), 1, line.size(), f);
        std::fputc('\n', f);
      }
      break;
    }
    case JournalRecord::Type::Text: {
      if (FILE* f = file_for(r.text_file[0] ? r.text_file : "text", wall_ns())) {
        std::fwrite(r.text.buf, 1, r.text.len, f);
        std::fputc('\n', f);
      }
      break;
    }
  }
}

void Journal::flush() {
  for (auto& [n, f] : files_)
    if (f) std::fflush(f);
}

void Journal::run(SpscQueue<JournalRecord>& q, Waker& waker, const std::atomic<bool>& stop) {
  uint64_t last_flush = mono_ns();
  for (;;) {
    bool any = false;
    while (JournalRecord* r = q.front()) {
      write(*r);
      q.pop();
      any = true;
    }
    const uint64_t now = mono_ns();
    if (now - last_flush > 250 * kNsPerMs) {
      flush();
      last_flush = now;
    }
    if (stop.load(std::memory_order_relaxed)) {
      while (JournalRecord* r = q.front()) {
        write(*r);
        q.pop();
      }
      flush();
      return;
    }
    if (!any) waker.wait(100 * kNsPerMs, [&] { return !q.empty(); });
  }
}

}  // namespace stok
