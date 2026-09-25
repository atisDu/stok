#include "check.hpp"
#include "engine/scorer.hpp"

using namespace stok;

namespace {

const Scorer& scorer() {
  static Scorer s = [] {
    Scorer sc;
    std::string err;
    if (!sc.load_rules_file(test::source_path("config/rules.tsv"), &err)) test::fail(__FILE__, __LINE__, err);
    return sc;
  }();
  return s;
}

}  // namespace

TEST(rules_file_compiles) {
  CHECK(scorer().rule_count() > 250);
  CHECK(scorer().state_count() > 100);
  Scorer bad;
  std::string err;
  CHECK(!bad.load_rules("cat:nope\t10\tphrase\n", &err));
  CHECK(!err.empty());
}

TEST(score_material_contract_with_tier1) {
  auto r = scorer().score_text(
      "Acme Robotics Signs $20 Million Supply Agreement with Walmart",
      "Acme Robotics, Inc. (NASDAQ: ACMR) today announced that it has signed a definitive three-year supply "
      "agreement with Walmart Inc. valued at $20 million.");
  CHECK(r.catalyst == Catalyst::Contract);
  CHECK(r.flags & kSfTier1);
  CHECK(!(r.flags & (kSfOffering | kSfSpam | kSfNonBinding)));
  CHECK_NEAR(r.amount_usd, 20e6, 1);
  CHECK(r.text_score >= 60);
  Scorer::finalize(r, 15e6);  // $20M on a $15M company
  CHECK(r.materiality > 1.0);
  CHECK(r.score >= r.text_score + 20 || r.score == 100);
}

TEST(score_fda_approval_is_high) {
  auto r = scorer().score_text("Delta Therapeutics Receives FDA Approval for DLX-101",
                               "the U.S. Food and Drug Administration has approved DLX-101");
  CHECK(r.catalyst == Catalyst::FdaApproval);
  CHECK(r.text_score >= 70);
}

TEST(score_offering_is_blocked) {
  auto r = scorer().score_text("Bravo Bio Announces Pricing of $5 Million Registered Direct Offering",
                               "priced at $2.00 per share for gross proceeds of $5 million");
  CHECK(r.flags & kSfOffering);
  CHECK(r.catalyst == Catalyst::Offering);
  CHECK(r.text_score <= 15);
}

TEST(score_law_firm_spam_is_zero) {
  auto r = scorer().score_text("INVESTOR ALERT: Rosen Law Firm Encourages Charlie Corp Investors to Secure Counsel",
                               "class action lawsuit deadline");
  CHECK(r.flags & kSfSpam);
  CHECK_EQ(r.text_score, 0);
}

TEST(score_promotional_loi_is_penalized) {
  auto promo = scorer().score_text(
      "Echo Mining Signs Letter of Intent to Explore Potential Lithium Partnership",
      "is thrilled to announce a non-binding letter of intent that could potentially lead to a game-changing "
      "partnership in a $50 billion market.");
  CHECK(promo.flags & kSfNonBinding);
  CHECK(promo.flags & kSfPromo);
  CHECK(promo.flags & kSfHedged);
  CHECK(promo.flags & kSfTamAmount);   // "$50 billion market" is market size, not deal value
  CHECK_EQ(promo.amount_usd, 0.0);
  CHECK(promo.text_score < 30);
}

TEST(score_negative_outcome) {
  auto r = scorer().score_text("Zeta Bio Phase 3 Trial Did Not Meet Primary Endpoint", "");
  CHECK(r.flags & kSfNegative);
  CHECK(r.text_score < 40);
}

TEST(score_word_boundaries_and_prefixes) {
  // "mou" must not match inside "amount"; "approv*" matches "approves".
  auto r = scorer().score_text("FDA approves Acme device in a large amount of time", "");
  CHECK(!(r.flags & kSfNonBinding));
  CHECK(r.catalyst == Catalyst::FdaApproval);
  // Hyphens and punctuation act as spaces.
  auto a = scorer().score_text("Company enters at-the-market equity program", "");
  CHECK(a.flags & kSfOffering);
}

TEST(amount_extraction) {
  uint32_t f = 0;
  CHECK_NEAR(Scorer::extract_amount("a $20.5M order", &f), 20.5e6, 1);
  CHECK_NEAR(Scorer::extract_amount("valued at US$3,000,000 over", &f), 3e6, 1);
  CHECK_NEAR(Scorer::extract_amount("$1.2 billion deal", &f), 1.2e9, 1);
  CHECK_EQ(Scorer::extract_amount("at $2.50 per share", &f), 0.0);
  f = 0;
  CHECK_NEAR(Scorer::extract_amount("contracts worth up to $10 million", &f), 10e6, 1);
  CHECK(f & kSfUpTo);
  f = 0;
  CHECK_EQ(Scorer::extract_amount("targets the $4 billion addressable market", &f), 0.0);
  CHECK(f & kSfTamAmount);
}
