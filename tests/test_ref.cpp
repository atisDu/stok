#include "check.hpp"
#include "fixture_symbols.hpp"
#include "ref/filings.hpp"
#include "util/time.hpp"

using namespace stok;

TEST(symbol_table_from_official_files) {
  const SymbolTable t = test::fixture_symbols();
  const uint32_t acmr = t.find("ACMR");
  CHECK(acmr != SymbolTable::kInvalid);
  if (acmr == SymbolTable::kInvalid) return;
  CHECK(t[acmr].exchange == Exchange::NasdaqCM);
  CHECK_EQ(t[acmr].cik, 1234567u);
  CHECK_NEAR(t[acmr].shares_outstanding, 12e6, 1);
  CHECK_NEAR(t[acmr].prev_close, 2.0, 1e-9);
  CHECK_EQ(t.find_key(SymbolTable::make_key("ACMR")), acmr);
  CHECK(!t[acmr].is_derivative_security());
  CHECK(t[t.find("ACMRW")].is_derivative_security());
  CHECK_EQ(t.primary_for_cik(1234567), acmr);
  CHECK(t[t.find("BRVO")].distressed());               // Financial Status D
  CHECK(t[t.find("QQQX")].etf);
  CHECK(t[t.find("ZZZT")].test_issue);
  CHECK(t[t.find("FOXT")].exchange == Exchange::NYSEAmerican);
  // SEC uses BRK-B, Nasdaq Trader BRK.B: both resolve to one symbol.
  CHECK(t.find("BRK-B") != SymbolTable::kInvalid && t.find("BRK-B") == t.find("brk.b"));
  CHECK_EQ(t[t.find("BRK.B")].cik, 1067983u);
  // OTC names come from the SEC map only.
  CHECK(t[t.find("ECMGF")].exchange == Exchange::OTC);
  CHECK(!is_exchange_listed(t[t.find("ECMGF")].exchange));
}

TEST(finra_short_volume) {
  SymbolTable t = test::fixture_symbols();
  const std::size_t n = t.load_finra_short_volume(
      "Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market\n"
      "20260924|ACMR|40000|0|100000|B,Q,N\n"
      "20260924|NOPE|1|0|2|Q\n");
  CHECK_EQ(n, 1u);
  CHECK_NEAR(t[t.find("ACMR")].short_volume_ratio, 0.4, 1e-9);
}

TEST(filings_history_master_index_and_flags) {
  FilingsHistory h;
  const std::size_t added = h.add_master_index(test::fixture("master.20260924.idx"));
  CHECK_EQ(added, 3u);  // 424B5, S-3, PRE 14A (Form 4 and 10-Q are ignored)
  const int32_t sep25 = static_cast<int32_t>(timeutil::days_from_civil(2026, 9, 25));
  const uint32_t f = h.flags(7654321, sep25);
  CHECK(f & kFlagOfferingRecent);
  CHECK(f & kFlagShelf);
  CHECK(!(h.flags(7654321, sep25 + 30) & kFlagOfferingRecent));  // offering window passed
  CHECK(h.flags(3333333, sep25) & kFlagReverseSplitRisk);
  CHECK_EQ(h.flags(2222222, sep25), 0u);
  // Live 8-K item 3.02 marks unregistered sales.
  h.add(1234567, sep25, FormClass::Form8K, kItem302);
  CHECK(h.flags(1234567, sep25) & kFlagUnregisteredSale);
  // Round trip through TSV.
  FilingsHistory h2;
  CHECK_EQ(h2.load_tsv(h.to_tsv()), 4u);
  CHECK_EQ(h2.flags(7654321, sep25), f);
  CHECK_EQ(dilution_flags_str(kFlagOfferingRecent | kFlagShelf), std::string("offering,shelf"));
}

TEST(form_classification) {
  CHECK(classify_form("424B5") == FormClass::Prospectus);
  CHECK(classify_form("S-1/A") == FormClass::S1);
  CHECK(classify_form("F-3") == FormClass::S3);
  CHECK(classify_form("EFFECT") == FormClass::Effect);
  CHECK(classify_form("8-K") == FormClass::Form8K);
  CHECK(classify_form("6-K") == FormClass::Form6K);
  CHECK(classify_form("NT 10-Q") == FormClass::NT);
  CHECK(classify_form("SC 13G") == FormClass::Other);
  CHECK_EQ(form8k_item_bit(1, 1), 0);
  CHECK_EQ(form8k_item_bit(9, 1), 31);
  CHECK_EQ(form8k_item_bit(6, 6), -1);
}
