#include "engine/scorer.hpp"

#include <algorithm>
#include <array>
#include <deque>

#include "core/ascii.hpp"
#include "util/file.hpp"

namespace stok {

namespace {

constexpr std::array<uint8_t, 256> make_class() {
  std::array<uint8_t, 256> t{};
  for (int c = '0'; c <= '9'; ++c) t[c] = static_cast<uint8_t>(1 + c - '0');
  for (int c = 'a'; c <= 'z'; ++c) t[c] = static_cast<uint8_t>(11 + c - 'a');
  for (int c = 'A'; c <= 'Z'; ++c) t[c] = static_cast<uint8_t>(11 + c - 'A');
  return t;
}
constexpr auto kClass = make_class();

struct CatName {
  Catalyst c;
  const char* name;
};
constexpr CatName kCatNames[] = {
    {Catalyst::None, "none"},           {Catalyst::FdaApproval, "fda"},      {Catalyst::ClinicalData, "clinical"},
    {Catalyst::Contract, "contract"},   {Catalyst::MnaTarget, "mna_target"}, {Catalyst::MnaAcquirer, "mna_acquirer"},
    {Catalyst::Earnings, "earnings"},   {Catalyst::Uplisting, "uplisting"},  {Catalyst::Partnership, "partnership"},
    {Catalyst::Product, "product"},     {Catalyst::Buyback, "buyback"},      {Catalyst::CryptoAi, "crypto_ai"},
    {Catalyst::LegalWin, "legal_win"},  {Catalyst::Offering, "offering"},    {Catalyst::ReverseSplit, "reverse_split"},
    {Catalyst::Distress, "distress"},   {Catalyst::Other, "other"},
};

}  // namespace

const char* catalyst_name(Catalyst c) {
  for (const auto& n : kCatNames)
    if (n.c == c) return n.name;
  return "?";
}

Catalyst parse_catalyst(std::string_view s) {
  for (const auto& n : kCatNames)
    if (iequals(s, n.name)) return n.c;
  return Catalyst::Count;
}

std::string score_flags_str(uint32_t f) {
  static const char* kNames[] = {"offering", "distress", "reverse_split", "spam",     "non_binding", "tier1",
                                 "up_to",    "promo",    "hedged",        "negative", "tam_amount"};
  std::string out;
  for (int i = 0; i < 11; ++i) {
    if (!(f & (1u << i))) continue;
    if (!out.empty()) out += ',';
    out += kNames[i];
  }
  return out;
}

bool Scorer::load_rules_file(const std::string& path, std::string* err) {
  auto t = fileutil::read_file(path);
  if (!t) {
    if (err) *err = "cannot read rules file " + path;
    return false;
  }
  return load_rules(*t, err);
}

bool Scorer::load_rules(std::string_view tsv, std::string* err) {
  rules_.clear();
  int lineno = 0;
  bool ok = true;
  for_each_line(tsv, [&](std::string_view line) {
    ++lineno;
    line = trim(line);
    if (line.empty() || line[0] == '#') return;
    std::string_view f[3];
    int n = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size() && n < 3; ++i) {
      if (i == line.size() || line[i] == '\t') {
        if (n < 2) {
          f[n++] = trim(line.substr(start, i - start));
          start = i + 1;
        } else {
          f[n++] = trim(line.substr(start));
          break;
        }
      }
    }
    if (n < 3 || f[2].empty()) {
      if (err && ok) *err = "rules line " + std::to_string(lineno) + ": expected kind<TAB>weight<TAB>phrase";
      ok = false;
      return;
    }
    Rule r;
    r.kind_str = std::string(f[0]);
    r.phrase = to_lower_copy(f[2]);
    r.weight = parse_int<int>(f[1]).value_or(0);
    if (istarts_with(f[0], "cat:")) {
      r.kind = kCat;
      r.cat = parse_catalyst(f[0].substr(4));
      if (r.cat == Catalyst::Count) {
        if (err && ok) *err = "rules line " + std::to_string(lineno) + ": unknown catalyst " + std::string(f[0]);
        ok = false;
        return;
      }
    } else if (f[0] == "spec") r.kind = kSpec;
    else if (f[0] == "hedge") r.kind = kHedge;
    else if (f[0] == "promo") r.kind = kPromo;
    else if (f[0] == "tier1") r.kind = kTier1;
    else if (f[0] == "nonbinding") r.kind = kNonBinding;
    else if (f[0] == "dilution") r.kind = kDilution;
    else if (f[0] == "distress") r.kind = kDistress;
    else if (f[0] == "rsplit") r.kind = kRsplit;
    else if (f[0] == "spam") r.kind = kSpam;
    else if (f[0] == "neg") r.kind = kNeg;
    else {
      if (err && ok) *err = "rules line " + std::to_string(lineno) + ": unknown kind " + std::string(f[0]);
      ok = false;
      return;
    }
    rules_.push_back(std::move(r));
  });
  if (!ok) return false;
  if (rules_.size() >= 65535) {
    if (err) *err = "too many rules";
    return false;
  }
  build();
  if (n_states_ >= 65535) {
    if (err) *err = "rule automaton too large";
    return false;
  }
  return true;
}

void Scorer::build() {
  // Normalize patterns to class sequences with boundary separators.
  norm_.clear();
  for (const Rule& r : rules_) {
    std::string p = r.phrase;
    bool prefix = false;
    if (!p.empty() && p.back() == '*') {
      prefix = true;
      p.pop_back();
    }
    std::string n = " ";
    for (char c : p) {
      if (kClass[static_cast<unsigned char>(c)]) n.push_back(to_lower(c));
      else if (n.back() != ' ') n.push_back(' ');
    }
    if (!prefix && n.back() != ' ') n.push_back(' ');
    if (prefix) while (n.size() > 1 && n.back() == ' ') n.pop_back();
    norm_.push_back(n);
  }
  // Trie.
  std::vector<std::array<int32_t, kAlpha>> go(1);
  go[0].fill(-1);
  std::vector<std::vector<uint16_t>> own(1);
  for (std::size_t id = 0; id < norm_.size(); ++id) {
    int s = 0;
    for (char ch : norm_[id]) {
      const int c = kClass[static_cast<unsigned char>(ch)];
      if (go[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)] < 0) {
        go[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)] = static_cast<int32_t>(go.size());
        go.emplace_back();
        go.back().fill(-1);
        own.emplace_back();
      }
      s = go[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)];
    }
    own[static_cast<std::size_t>(s)].push_back(static_cast<uint16_t>(id));
  }
  n_states_ = go.size();
  // BFS: failure links, full DFA, merged outputs.
  std::vector<int32_t> fail(n_states_, 0);
  delta_.assign(n_states_ * kAlpha, 0);
  std::vector<std::vector<uint16_t>> outs(n_states_);
  std::deque<int32_t> q;
  for (int c = 0; c < kAlpha; ++c) {
    const int32_t t = go[0][static_cast<std::size_t>(c)];
    if (t > 0) {
      delta_[static_cast<std::size_t>(c)] = static_cast<uint16_t>(t);
      fail[static_cast<std::size_t>(t)] = 0;
      q.push_back(t);
    } else {
      delta_[static_cast<std::size_t>(c)] = 0;
    }
  }
  outs[0] = own[0];
  std::vector<int32_t> order;
  while (!q.empty()) {
    const int32_t s = q.front();
    q.pop_front();
    order.push_back(s);
    auto& o = outs[static_cast<std::size_t>(s)];
    o = own[static_cast<std::size_t>(s)];
    const auto& fo = outs[static_cast<std::size_t>(fail[static_cast<std::size_t>(s)])];
    o.insert(o.end(), fo.begin(), fo.end());
    for (int c = 0; c < kAlpha; ++c) {
      const int32_t t = go[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)];
      const std::size_t idx = static_cast<std::size_t>(s) * kAlpha + static_cast<std::size_t>(c);
      if (t > 0) {
        fail[static_cast<std::size_t>(t)] = delta_[static_cast<std::size_t>(fail[static_cast<std::size_t>(s)]) * kAlpha + static_cast<std::size_t>(c)];
        delta_[idx] = static_cast<uint16_t>(t);
        q.push_back(t);
      } else {
        delta_[idx] = delta_[static_cast<std::size_t>(fail[static_cast<std::size_t>(s)]) * kAlpha + static_cast<std::size_t>(c)];
      }
    }
  }
  out_begin_.assign(n_states_ + 1, 0);
  out_ids_.clear();
  for (std::size_t s = 0; s < n_states_; ++s) {
    out_begin_[s] = static_cast<uint32_t>(out_ids_.size());
    out_ids_.insert(out_ids_.end(), outs[s].begin(), outs[s].end());
  }
  out_begin_[n_states_] = static_cast<uint32_t>(out_ids_.size());
}

void Scorer::scan(std::string_view text, std::vector<uint16_t>& hits) const {
  if (n_states_ == 0) return;
  const uint16_t* d = delta_.data();
  const uint32_t* ob = out_begin_.data();
  const uint16_t* oi = out_ids_.data();
  uint32_t s = d[0];  // leading separator
  bool prev_sep = true;
  auto emit = [&](uint32_t st) {
    for (uint32_t k = ob[st]; k < ob[st + 1]; ++k) hits.push_back(oi[k]);
  };
  for (const char ch : text) {
    const uint8_t c = kClass[static_cast<unsigned char>(ch)];
    if (c == 0) {
      if (prev_sep) continue;
      prev_sep = true;
    } else {
      prev_sep = false;
    }
    s = d[s * kAlpha + c];
    if (ob[s] != ob[s + 1]) emit(s);
  }
  if (!prev_sep) {
    s = d[s * kAlpha];
    if (ob[s] != ob[s + 1]) emit(s);
  }
}

double Scorer::extract_amount(std::string_view t, uint32_t* flags) {
  double best = 0;
  std::size_t pos = 0;
  while (pos < t.size()) {
    const std::size_t dollar = t.find('$', pos);
    if (dollar == std::string_view::npos) break;
    pos = dollar + 1;
    std::size_t i = dollar + 1;
    while (i < t.size() && t[i] == ' ') ++i;
    const std::size_t nb = i;
    double v = 0, frac = 0, scale = 0.1;
    bool any = false, in_frac = false;
    while (i < t.size()) {
      const char c = t[i];
      if (is_digit(c)) {
        any = true;
        if (in_frac) {
          frac += (c - '0') * scale;
          scale /= 10;
        } else {
          v = v * 10 + (c - '0');
        }
      } else if (c == ',' && !in_frac && i + 1 < t.size() && is_digit(t[i + 1])) {
        // thousands separator
      } else if (c == '.' && !in_frac && i + 1 < t.size() && is_digit(t[i + 1])) {
        in_frac = true;
      } else {
        break;
      }
      ++i;
    }
    if (!any || i == nb) continue;
    v += frac;
    // Multiplier word.
    std::size_t j = i;
    while (j < t.size() && t[j] == ' ') ++j;
    std::size_t we = j;
    while (we < t.size() && is_alpha(t[we])) ++we;
    const std::string_view w = t.substr(j, we - j);
    double mult = 1;
    if (iequals(w, "million") || iequals(w, "mm") || iequals(w, "mln") || iequals(w, "m")) mult = 1e6;
    else if (iequals(w, "billion") || iequals(w, "bn") || iequals(w, "b")) mult = 1e9;
    else if (iequals(w, "thousand") || iequals(w, "k")) mult = 1e3;
    else if (iequals(w, "trillion") || iequals(w, "tn")) mult = 1e12;
    const std::size_t after = mult > 1 ? we : i;
    const std::string_view tail = t.substr(after, std::min<std::size_t>(60, t.size() - after));
    // "$2.50 per share" is a price, not an amount.
    if (ifind(tail.substr(0, std::min<std::size_t>(tail.size(), 12)), "per ") != std::string_view::npos ||
        ifind(tail.substr(0, std::min<std::size_t>(tail.size(), 6)), "/sh") != std::string_view::npos)
      continue;
    // Market-size figures ("$50 billion market") inflate materiality: skip.
    const std::string_view near = tail.substr(0, std::min<std::size_t>(tail.size(), 45));
    if (ifind(near, " market") != std::string_view::npos || ifind(near, "industry") != std::string_view::npos ||
        ifind(near, "addressable") != std::string_view::npos || ifind(near, " tam") != std::string_view::npos ||
        ifind(near, "opportunity") != std::string_view::npos || ifind(near, " sector") != std::string_view::npos) {
      if (flags) *flags |= kSfTamAmount;
      continue;
    }
    const std::size_t lb = dollar >= 12 ? dollar - 12 : 0;
    const std::string_view before = t.substr(lb, dollar - lb);
    if (ifind(before, "up to") != std::string_view::npos && flags) *flags |= kSfUpTo;
    best = std::max(best, v * mult);
    pos = after;
  }
  return best;
}

ScoreResult Scorer::score_text(std::string_view title, std::string_view body) const {
  ScoreResult r;
  thread_local std::vector<uint16_t> hits_title, hits_body;
  thread_local std::vector<uint8_t> seen;
  hits_title.clear();
  hits_body.clear();
  scan(title, hits_title);
  scan(body, hits_body);
  if (seen.size() < rules_.size()) seen.assign(rules_.size(), 0);

  int best_cat_w = 0;
  Catalyst best_cat = Catalyst::None;
  int spec = 0, hedge = 0, promo = 0, tier1 = 0, nonbinding = 0, neg = 0;
  auto consider = [&](uint16_t id, bool in_title) {
    const uint8_t bit = in_title ? 1 : 2;
    if (seen[id] & bit) return;
    const bool first_time = seen[id] == 0;
    seen[id] |= bit;
    const Rule& rule = rules_[id];
    if (first_time && r.n_matched < 12) r.matched[r.n_matched++] = id;
    switch (rule.kind) {
      case kCat: {
        const int w = rule.weight + (in_title ? 5 : 0);
        if (w > best_cat_w) {
          best_cat_w = w;
          best_cat = rule.cat;
        }
        break;
      }
      case kSpec:
        if (first_time) {
          spec += rule.weight;
          ++r.specifics;
        }
        break;
      case kHedge:
        if (first_time) {
          hedge += rule.weight;
          ++r.hedges;
          r.flags |= kSfHedged;
        }
        break;
      case kPromo:
        if (first_time) {
          promo += rule.weight;
          ++r.promos;
          r.flags |= kSfPromo;
        }
        break;
      case kTier1:
        tier1 = std::max(tier1, rule.weight);
        r.flags |= kSfTier1;
        break;
      case kNonBinding:
        nonbinding = std::min(nonbinding, rule.weight);
        r.flags |= kSfNonBinding;
        break;
      case kDilution: r.flags |= kSfOffering; break;
      case kDistress: r.flags |= kSfDistress; break;
      case kRsplit: r.flags |= kSfReverseSplit; break;
      case kSpam: r.flags |= kSfSpam; break;
      case kNeg:
        if (first_time) {
          neg += rule.weight;
          r.flags |= kSfNegative;
        }
        break;
      default: break;
    }
  };
  for (uint16_t id : hits_title) consider(id, true);
  for (uint16_t id : hits_body) consider(id, false);
  for (uint16_t id : hits_title) seen[id] = 0;
  for (uint16_t id : hits_body) seen[id] = 0;

  r.catalyst = best_cat;
  int s = best_cat_w + std::min(spec, 15) + tier1 + std::max(hedge, -15) + std::max(promo, -15) + nonbinding +
          std::max(neg, -30);
  if (r.flags & kSfReverseSplit) {
    s -= 20;
    if (best_cat_w < 40) r.catalyst = Catalyst::ReverseSplit;
  }
  if (r.flags & kSfDistress) {
    s -= 25;
    if (best_cat_w < 40) r.catalyst = Catalyst::Distress;
  }
  if (r.flags & kSfOffering) {
    r.catalyst = Catalyst::Offering;
    s = std::min(s, 15);
  }
  if (r.flags & kSfSpam) s = 0;

  uint32_t amt_flags = 0;
  const double a_title = extract_amount(title, &amt_flags);
  const double a_body = extract_amount(body.substr(0, std::min<std::size_t>(body.size(), 1500)), &amt_flags);
  r.amount_usd = std::max(a_title, a_body);
  r.flags |= amt_flags;
  r.text_score = std::clamp(s, 0, 100);
  r.score = r.text_score;
  return r;
}

void Scorer::finalize(ScoreResult& r, double market_cap) {
  int pts = 0;
  r.materiality = 0;
  const bool applies = r.catalyst != Catalyst::Offering && r.catalyst != Catalyst::Distress &&
                       r.catalyst != Catalyst::ReverseSplit && r.catalyst != Catalyst::MnaAcquirer &&
                       r.catalyst != Catalyst::None && !(r.flags & kSfSpam);
  if (market_cap > 0 && r.amount_usd > 0) {
    r.materiality = r.amount_usd / market_cap;
    if (applies) {
      if (r.materiality >= 1.0) pts = 20;
      else if (r.materiality >= 0.3) pts = 12;
      else if (r.materiality >= 0.1) pts = 6;
      else if (r.materiality < 0.02) pts = -5;
      if (pts > 0 && (r.flags & kSfUpTo)) pts /= 2;
    }
  }
  r.score = std::clamp(r.text_score + pts, 0, 100);
}

}  // namespace stok
