#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stok {

enum class Catalyst : uint8_t {
  None = 0,
  FdaApproval,
  ClinicalData,
  Contract,
  MnaTarget,     // company is being acquired (price usually pins to the deal)
  MnaAcquirer,
  Earnings,
  Uplisting,
  Partnership,
  Product,
  Buyback,
  CryptoAi,      // "pivot" buzzword announcements
  LegalWin,
  Offering,
  ReverseSplit,
  Distress,
  Other,
  Count
};

const char* catalyst_name(Catalyst c);
Catalyst parse_catalyst(std::string_view s);

// Text flags produced by the scorer.
enum ScoreFlag : uint32_t {
  kSfOffering = 1u << 0,      // offering / financing language (dilution): hard block
  kSfDistress = 1u << 1,      // delisting notice, going concern, bankruptcy, default
  kSfReverseSplit = 1u << 2,  // reverse split announced
  kSfSpam = 1u << 3,          // law-firm "investor alert" and similar
  kSfNonBinding = 1u << 4,    // LOI / MOU / non-binding
  kSfTier1 = 1u << 5,         // named top-tier counterparty
  kSfUpTo = 1u << 6,          // "up to $X": headline amount is a ceiling
  kSfPromo = 1u << 7,         // promotional language present
  kSfHedged = 1u << 8,        // hedging language present
  kSfNegative = 1u << 9,      // negative outcome words (missed endpoint, terminated...)
  kSfTamAmount = 1u << 10,    // dollar figure refers to a market size, ignored
};

std::string score_flags_str(uint32_t f);

struct ScoreResult {
  int text_score = 0;   // 0..100 before materiality
  int score = 0;        // final 0..100 after finalize()
  Catalyst catalyst = Catalyst::None;
  uint32_t flags = 0;
  double amount_usd = 0;       // largest non-market-size dollar amount found
  double materiality = 0;      // amount / market cap (finalize)
  int16_t hedges = 0, promos = 0, specifics = 0;
  uint8_t n_matched = 0;
  uint16_t matched[12];        // rule ids, for explanations
};

// Rule-based news scorer. All phrases from the rules file are compiled into
// one Aho-Corasick DFA over a 37-symbol alphabet (a-z, 0-9, separator), so a
// document is scored in a single pass with one table lookup per byte,
// however many rules there are. Word boundaries come from the separator
// symbol; a trailing '*' makes a rule a prefix match ("approv*").
//
// Rules file (TSV):  kind <TAB> weight <TAB> phrase
//   kind = cat:<catalyst> | spec | hedge | promo | tier1 | nonbinding |
//          dilution | distress | rsplit | spam | neg
class Scorer {
 public:
  struct Rule {
    std::string kind_str;
    std::string phrase;
    int weight = 0;
    uint8_t kind = 0;
    Catalyst cat = Catalyst::None;
  };

  bool load_rules(std::string_view tsv, std::string* err);
  bool load_rules_file(const std::string& path, std::string* err);
  std::size_t rule_count() const { return rules_.size(); }
  std::size_t state_count() const { return n_states_; }
  const Rule& rule(uint16_t id) const { return rules_[id]; }

  // Scores title + body. Title matches count double for catalysts.
  ScoreResult score_text(std::string_view title, std::string_view body) const;
  // Applies materiality (amount vs market cap) and clamps. market_cap <= 0
  // means unknown (no materiality points).
  static void finalize(ScoreResult& r, double market_cap_usd);

  // Largest dollar amount in `text` that isn't a market-size figure.
  static double extract_amount(std::string_view text, uint32_t* flags);

 private:
  enum Kind : uint8_t { kCat = 1, kSpec, kHedge, kPromo, kTier1, kNonBinding, kDilution, kDistress, kRsplit, kSpam, kNeg };
  static constexpr int kAlpha = 37;

  void build();
  void scan(std::string_view text, std::vector<uint16_t>& hits) const;

  std::vector<Rule> rules_;
  std::vector<std::string> norm_;         // normalized patterns
  std::vector<uint16_t> delta_;           // n_states * kAlpha
  std::vector<uint32_t> out_begin_;       // per state, into out_ids_
  std::vector<uint16_t> out_ids_;
  std::size_t n_states_ = 0;
};

}  // namespace stok
