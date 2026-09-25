#include "ref/filings.hpp"

#include <algorithm>
#include <cstdio>

#include "core/ascii.hpp"
#include "util/time.hpp"

namespace stok {

namespace {

struct Item {
  int major, minor;
  const char* label;
  const char* title;
};

constexpr Item kItems[32] = {
    {1, 1, "1.01", "Entry into a Material Definitive Agreement"},
    {1, 2, "1.02", "Termination of a Material Definitive Agreement"},
    {1, 3, "1.03", "Bankruptcy or Receivership"},
    {1, 4, "1.04", "Mine Safety"},
    {1, 5, "1.05", "Material Cybersecurity Incidents"},
    {2, 1, "2.01", "Completion of Acquisition or Disposition of Assets"},
    {2, 2, "2.02", "Results of Operations and Financial Condition"},
    {2, 3, "2.03", "Creation of a Direct Financial Obligation"},
    {2, 4, "2.04", "Triggering Events That Accelerate a Financial Obligation"},
    {2, 5, "2.05", "Costs Associated with Exit or Disposal Activities"},
    {2, 6, "2.06", "Material Impairments"},
    {3, 1, "3.01", "Notice of Delisting or Failure to Satisfy a Listing Rule"},
    {3, 2, "3.02", "Unregistered Sales of Equity Securities"},
    {3, 3, "3.03", "Material Modification to Rights of Security Holders"},
    {4, 1, "4.01", "Changes in Certifying Accountant"},
    {4, 2, "4.02", "Non-Reliance on Previously Issued Financial Statements"},
    {5, 1, "5.01", "Changes in Control of Registrant"},
    {5, 2, "5.02", "Departure/Election of Directors or Officers"},
    {5, 3, "5.03", "Amendments to Articles of Incorporation or Bylaws"},
    {5, 4, "5.04", "Temporary Suspension of Trading Under Employee Benefit Plans"},
    {5, 5, "5.05", "Amendments to the Code of Ethics"},
    {5, 6, "5.06", "Change in Shell Company Status"},
    {5, 7, "5.07", "Submission of Matters to a Vote of Security Holders"},
    {5, 8, "5.08", "Shareholder Director Nominations"},
    {6, 1, "6.01", "ABS Informational and Computational Material"},
    {6, 2, "6.02", "Change of Servicer or Trustee"},
    {6, 3, "6.03", "Change in Credit Enhancement"},
    {6, 4, "6.04", "Failure to Make a Required Distribution"},
    {6, 5, "6.05", "Securities Act Updating Disclosure"},
    {7, 1, "7.01", "Regulation FD Disclosure"},
    {8, 1, "8.01", "Other Events"},
    {9, 1, "9.01", "Financial Statements and Exhibits"},
};

}  // namespace

int form8k_item_bit(int major, int minor) {
  for (int i = 0; i < 32; ++i)
    if (kItems[i].major == major && kItems[i].minor == minor) return i;
  return -1;
}
const char* form8k_item_label(int bit) { return bit >= 0 && bit < 32 ? kItems[bit].label : "?"; }
const char* form8k_item_title(int bit) { return bit >= 0 && bit < 32 ? kItems[bit].title : "?"; }

std::string form8k_items_str(uint32_t mask) {
  std::string out;
  for (int i = 0; i < 32; ++i) {
    if (!(mask & (1u << i))) continue;
    if (!out.empty()) out += ',';
    out += kItems[i].label;
  }
  return out;
}

FormClass classify_form(std::string_view f) {
  f = trim(f);
  auto is = [&](std::string_view x) { return iequals(f, x); };
  auto starts = [&](std::string_view x) { return istarts_with(f, x); };
  if (is("S-1") || is("S-1/A") || is("F-1") || is("F-1/A") || is("S-1MEF") || is("F-1MEF")) return FormClass::S1;
  if (is("S-3") || is("S-3/A") || is("F-3") || is("F-3/A") || is("S-3ASR") || is("F-3ASR") || is("S-3MEF"))
    return FormClass::S3;
  if (starts("424B")) return FormClass::Prospectus;
  if (is("EFFECT")) return FormClass::Effect;
  if (is("PRE 14A") || is("PRE 14C")) return FormClass::PreProxy;
  if (is("DEF 14A") || is("DEF 14C")) return FormClass::DefProxy;
  if (starts("S-8")) return FormClass::S8;
  if (is("8-K") || is("8-K/A")) return FormClass::Form8K;
  if (is("6-K") || is("6-K/A")) return FormClass::Form6K;
  if (is("10-Q") || is("10-Q/A")) return FormClass::Form10Q;
  if (is("10-K") || is("10-K/A") || is("20-F") || is("40-F")) return FormClass::Form10K;
  if (starts("NT 10-") || starts("NT 20-")) return FormClass::NT;
  return FormClass::Other;
}

const char* form_class_name(FormClass c) {
  static const char* kNames[] = {"other", "s1", "s3", "424b", "effect", "pre14", "def14",
                                 "s8",    "8k", "6k", "10q",  "10k",    "nt"};
  const auto i = static_cast<std::size_t>(c);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "other";
}

std::string dilution_flags_str(uint32_t f) {
  std::string out;
  auto add = [&](uint32_t bit, const char* name) {
    if (!(f & bit)) return;
    if (!out.empty()) out += ',';
    out += name;
  };
  add(kFlagOfferingRecent, "offering");
  add(kFlagEffectRecent, "effect");
  add(kFlagS1Pending, "s1");
  add(kFlagShelf, "shelf");
  add(kFlagReverseSplitRisk, "rs_proxy");
  add(kFlagLateFiler, "late_filer");
  add(kFlagUnregisteredSale, "item3.02");
  add(kFlagDeficiency, "deficiency");
  add(kFlagReverseSplitDone, "item5.03");
  return out;
}

void FilingsHistory::add(uint32_t cik, int32_t day, FormClass cls, uint32_t items) {
  if (cik == 0) return;
  auto& v = by_cik_[cik];
  for (auto& e : v) {
    if (e.day == day && e.cls == cls) {
      e.items |= items;
      return;
    }
  }
  v.push_back(Entry{day, cls, items});
}

const std::vector<FilingsHistory::Entry>* FilingsHistory::entries(uint32_t cik) const {
  auto it = by_cik_.find(cik);
  return it == by_cik_.end() ? nullptr : &it->second;
}

uint32_t FilingsHistory::flags(uint32_t cik, int32_t today, const DilutionPolicy& p) const {
  const auto* v = entries(cik);
  if (!v) return 0;
  uint32_t f = 0;
  for (const auto& e : *v) {
    const int age = today - e.day;
    if (age < 0) continue;
    switch (e.cls) {
      case FormClass::Prospectus:
        if (age <= p.offering_window_days) f |= kFlagOfferingRecent;
        break;
      case FormClass::Effect:
        if (age <= p.offering_window_days) f |= kFlagEffectRecent;
        break;
      case FormClass::S1:
        if (age <= p.s1_window_days) f |= kFlagS1Pending;
        break;
      case FormClass::S3:
        if (age <= p.shelf_window_days) f |= kFlagShelf;
        break;
      case FormClass::PreProxy:
        if (age <= p.proxy_window_days) f |= kFlagReverseSplitRisk;
        break;
      case FormClass::NT:
        if (age <= p.late_filer_days) f |= kFlagLateFiler;
        break;
      case FormClass::Form8K:
      case FormClass::Form6K:
        if ((e.items & kItem302) && age <= p.item302_days) f |= kFlagUnregisteredSale;
        if ((e.items & kItem301) && age <= p.item301_days) f |= kFlagDeficiency;
        if ((e.items & kItem503) && age <= p.item503_days) f |= kFlagReverseSplitDone;
        break;
      default:
        break;
    }
  }
  return f;
}

std::size_t FilingsHistory::add_master_index(std::string_view text) {
  std::size_t added = 0;
  bool in_data = false;
  for_each_line(text, [&](std::string_view line) {
    if (!in_data) {
      // Data starts after the dashed separator line.
      if (line.size() > 10 && line.find_first_not_of('-') == std::string_view::npos) in_data = true;
      return;
    }
    std::string_view f[5];
    int n = 0;
    split(line, '|', [&](std::string_view c) {
      if (n < 5) f[n++] = c;
    });
    if (n < 4) return;
    const FormClass cls = classify_form(f[2]);
    if (cls == FormClass::Other || cls == FormClass::Form10Q || cls == FormClass::Form10K ||
        cls == FormClass::DefProxy || cls == FormClass::S8)
      return;
    auto cik = parse_int<uint32_t>(f[0]);
    if (!cik) return;
    // Date Filed is YYYYMMDD in daily indexes (YYYY-MM-DD in some quarterly ones).
    std::string_view d = trim(f[3]);
    int y = 0;
    unsigned m = 0, dd = 0;
    if (d.size() == 8) {
      y = parse_int<int>(d.substr(0, 4)).value_or(0);
      m = parse_int<unsigned>(d.substr(4, 2)).value_or(0);
      dd = parse_int<unsigned>(d.substr(6, 2)).value_or(0);
    } else if (!timeutil::parse_ymd(d, y, m, dd)) {
      return;
    }
    if (y < 1990 || m < 1 || m > 12 || dd < 1 || dd > 31) return;
    add(*cik, static_cast<int32_t>(timeutil::days_from_civil(y, m, dd)), cls);
    ++added;
  });
  return added;
}

std::string FilingsHistory::to_tsv() const {
  std::string out = "cik\tdate\tclass\titems\n";
  char buf[96];
  for (const auto& [cik, v] : by_cik_) {
    for (const auto& e : v) {
      int y;
      unsigned m, d;
      timeutil::civil_from_days(e.day, y, m, d);
      std::snprintf(buf, sizeof(buf), "%u\t%04d-%02u-%02u\t%u\t%u\n", cik, y, m, d, static_cast<unsigned>(e.cls),
                    e.items);
      out += buf;
    }
  }
  return out;
}

std::size_t FilingsHistory::load_tsv(std::string_view tsv) {
  std::size_t n_added = 0;
  for_each_line(tsv, [&](std::string_view line) {
    if (line.empty() || istarts_with(line, "cik")) return;
    std::string_view f[4];
    int n = 0;
    split(line, '\t', [&](std::string_view c) {
      if (n < 4) f[n++] = c;
    });
    if (n < 3) return;
    auto cik = parse_int<uint32_t>(f[0]);
    auto cls = parse_int<unsigned>(f[2]);
    int y;
    unsigned m, d;
    if (!cik || !cls || *cls >= static_cast<unsigned>(FormClass::Count) || !timeutil::parse_ymd(f[1], y, m, d)) return;
    add(*cik, static_cast<int32_t>(timeutil::days_from_civil(y, m, d)), static_cast<FormClass>(*cls),
        n > 3 ? parse_int<uint32_t>(f[3]).value_or(0) : 0);
    ++n_added;
  });
  return n_added;
}

}  // namespace stok
