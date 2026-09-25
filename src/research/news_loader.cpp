#include "research/news_loader.hpp"

#include <algorithm>
#include <cstdio>

#include "core/ascii.hpp"
#include "ref/filings.hpp"
#include "util/file.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace stok::research {

namespace {

EventKind kind_of(std::string_view k) {
  if (k == "filing") return EventKind::Filing;
  if (k == "filing_doc") return EventKind::FilingDoc;
  if (k == "halt") return EventKind::Halt;
  return EventKind::News;
}

uint32_t items_mask(std::string_view items) {
  uint32_t mask = 0;
  split(items, ',', [&](std::string_view it) {
    it = trim(it);
    if (it.size() == 4 && is_digit(it[0]) && it[1] == '.' && is_digit(it[2]) && is_digit(it[3])) {
      const int bit = form8k_item_bit(it[0] - '0', (it[2] - '0') * 10 + (it[3] - '0'));
      if (bit >= 0) mask |= 1u << bit;
    }
  });
  return mask;
}

uint16_t source_index(std::vector<std::string>& sources, const std::string& name) {
  for (std::size_t i = 0; i < sources.size(); ++i)
    if (sources[i] == name) return static_cast<uint16_t>(i);
  sources.push_back(name);
  return static_cast<uint16_t>(sources.size() - 1);
}

}  // namespace

LoadedNews load_news_events(std::string_view jsonl, const SymbolTable& symbols, std::vector<std::string>& sources) {
  LoadedNews out;
  JsonDoc d;
  for_each_line(jsonl, [&](std::string_view line) {
    if (trim(line).empty()) return;
    ++out.lines;
    if (!d.parse(line) || d.root().type != JsonDoc::Type::Object) {
      ++out.bad_lines;
      return;
    }
    const auto& r = d.root();
    NewsEvent ev;
    ev.reset();
    ev.kind = kind_of(JsonDoc::str(d.get(r, "kind")));
    ev.source = source_index(sources, JsonDoc::str(d.get(r, "src")));
    ev.recv_ns = JsonDoc::u64(d.get(r, "recv_ns"));
    ev.sent_ns = JsonDoc::u64(d.get(r, "sent_ns"));
    ev.published_ns = JsonDoc::i64(d.get(r, "pub_ns"));
    ev.parsed_ns = ev.recv_ns;
    if (ev.recv_ns == 0) {
      ++out.bad_lines;
      return;
    }
    const std::string id = JsonDoc::str(d.get(r, "id"));
    ev.id_hash = std::strtoull(id.c_str(), nullptr, 16);
    ev.title.assign(JsonDoc::str(d.get(r, "title")));
    ev.link.assign(JsonDoc::str(d.get(r, "link")));
    ev.body.assign(JsonDoc::str(d.get(r, "body")));
    ev.title_key = text::headline_key(ev.title.view());
    if (const auto* tk = d.get(r, "tickers")) {
      d.for_each(*tk, [&](const JsonDoc::Node& n) {
        const uint32_t sym = symbols.find(JsonDoc::str(&n));
        if (sym == SymbolTable::kInvalid) {
          ++out.dropped_tickers;
          return;
        }
        if (ev.n_tickers < kMaxTickers) ev.tickers[ev.n_tickers++] = sym;
      });
    }
    if (const auto* un = d.get(r, "unresolved")) {
      d.for_each(*un, [&](const JsonDoc::Node& n) {
        if (ev.n_unresolved >= kMaxUnresolved) return;
        std::snprintf(ev.unresolved[ev.n_unresolved++], sizeof(ev.unresolved[0]), "%s", JsonDoc::str(&n).c_str());
      });
    }
    const auto* nm = d.get(r, "name_matched");
    if (nm && nm->type == JsonDoc::Type::Bool && nm->boolean) ev.flags |= kEvNameMatched;
    std::snprintf(ev.form, sizeof(ev.form), "%s", JsonDoc::str(d.get(r, "form")).c_str());
    ev.cik = static_cast<uint32_t>(JsonDoc::u64(d.get(r, "cik")));
    ev.items_mask = items_mask(JsonDoc::str(d.get(r, "items")));
    if (ev.kind == EventKind::Halt) {
      std::snprintf(ev.halt.reason, sizeof(ev.halt.reason), "%s", JsonDoc::str(d.get(r, "halt_reason")).c_str());
      std::snprintf(ev.halt.market, sizeof(ev.halt.market), "%s", JsonDoc::str(d.get(r, "market")).c_str());
      ev.halt.halt_ns = JsonDoc::i64(d.get(r, "halt_ns"));
      ev.halt.resume_quote_ns = JsonDoc::i64(d.get(r, "resume_quote_ns"));
      ev.halt.resume_trade_ns = JsonDoc::i64(d.get(r, "resume_trade_ns"));
      ev.halt.pause_threshold = JsonDoc::num(d.get(r, "pause_threshold"));
    }
    out.events.push_back(ev);
  });
  std::stable_sort(out.events.begin(), out.events.end(),
                   [](const NewsEvent& a, const NewsEvent& b) { return a.recv_ns < b.recv_ns; });
  return out;
}

LoadedNews load_news_file(const std::string& path, const SymbolTable& symbols, std::vector<std::string>& sources) {
  auto text = fileutil::read_file(path);
  if (!text) return {};
  return load_news_events(*text, symbols, sources);
}

}  // namespace stok::research
