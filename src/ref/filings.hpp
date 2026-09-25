#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stok {

// Filing types that matter for dilution / distress screening.
enum class FormClass : uint8_t {
  Other = 0,
  S1,           // S-1, S-1/A, F-1, F-1/A: registration (IPO / follow-on)
  S3,           // S-3, S-3/A, F-3, S-3ASR: shelf registration
  Prospectus,   // 424B1..424B8: a takedown / priced offering
  Effect,       // EFFECT: SEC declared a registration effective
  PreProxy,     // PRE 14A/PRE 14C: preliminary proxy (often reverse-split votes)
  DefProxy,     // DEF 14A
  S8,           // employee plan shares
  Form8K,
  Form6K,
  Form10Q,
  Form10K,
  NT,           // NT 10-K / NT 10-Q: late filing notice
  Count
};

FormClass classify_form(std::string_view form);

// 8-K items as a 32-bit mask (one bit per item number, 1.01 .. 9.01).
int form8k_item_bit(int major, int minor);  // -1 if unknown
const char* form8k_item_label(int bit);     // "1.01"
const char* form8k_item_title(int bit);     // "Entry into a Material Definitive Agreement"
std::string form8k_items_str(uint32_t mask);  // "1.01,3.02"
inline constexpr uint32_t kItem101 = 1u << 0;   // material definitive agreement
inline constexpr uint32_t kItem103 = 1u << 2;   // bankruptcy
inline constexpr uint32_t kItem201 = 1u << 5;   // completed acquisition/disposition
inline constexpr uint32_t kItem202 = 1u << 6;   // results of operations
inline constexpr uint32_t kItem301 = 1u << 11;  // delisting notice / listing deficiency
inline constexpr uint32_t kItem302 = 1u << 12;  // unregistered equity sales
inline constexpr uint32_t kItem402 = 1u << 15;  // non-reliance (restatement)
inline constexpr uint32_t kItem501 = 1u << 16;  // change in control
inline constexpr uint32_t kItem503 = 1u << 18;  // charter amendments (reverse splits)
inline constexpr uint32_t kItem701 = 1u << 29;  // Reg FD
inline constexpr uint32_t kItem801 = 1u << 30;  // other events
const char* form_class_name(FormClass c);

// Dilution / distress flags derived from recent filings.
enum DilutionFlag : uint32_t {
  kFlagOfferingRecent = 1u << 0,   // 424B within offering window
  kFlagEffectRecent = 1u << 1,     // EFFECT within offering window: offering can price now
  kFlagS1Pending = 1u << 2,        // S-1/F-1 within 180 days
  kFlagShelf = 1u << 3,            // S-3/F-3 within shelf window (default 3 years or history length)
  kFlagReverseSplitRisk = 1u << 4, // preliminary proxy within 90 days
  kFlagLateFiler = 1u << 5,        // NT 10-K/Q within 120 days
  kFlagUnregisteredSale = 1u << 6, // 8-K item 3.02 within 30 days (live feed)
  kFlagDeficiency = 1u << 7,       // 8-K item 3.01 within 180 days, or exchange financial status flag
  kFlagReverseSplitDone = 1u << 8, // 8-K item 5.03 within 60 days (often the split itself)
};

std::string dilution_flags_str(uint32_t flags);

struct DilutionPolicy {
  int offering_window_days = 10;
  int s1_window_days = 180;
  int shelf_window_days = 3 * 365;
  int proxy_window_days = 90;
  int late_filer_days = 120;
  int item302_days = 30;
  int item301_days = 180;
  int item503_days = 60;
};

// Per-issuer record of recent relevant filings, bootstrapped from EDGAR's
// official daily form indexes and kept current from the live EDGAR feed.
class FilingsHistory {
 public:
  struct Entry {
    int32_t day;  // days since epoch
    FormClass cls;
    uint32_t items;  // 8-K item bits, when known
  };

  void add(uint32_t cik, int32_t day, FormClass cls, uint32_t items = 0);
  uint32_t flags(uint32_t cik, int32_t today, const DilutionPolicy& p = {}) const;
  const std::vector<Entry>* entries(uint32_t cik) const;
  std::size_t issuers() const { return by_cik_.size(); }

  // EDGAR daily-index "master.YYYYMMDD.idx" (pipe-delimited:
  // CIK|Company Name|Form Type|Date Filed|Filename). Adds relevant rows.
  std::size_t add_master_index(std::string_view text);

  // Compact TSV persistence: cik \t YYYY-MM-DD \t form_class \t items
  std::string to_tsv() const;
  std::size_t load_tsv(std::string_view tsv);

 private:
  std::unordered_map<uint32_t, std::vector<Entry>> by_cik_;
};

}  // namespace stok
