#include "util/time.hpp"

#include <cstdio>

#include "core/ascii.hpp"

namespace stok::timeutil {

void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) noexcept {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}

namespace {

constexpr int64_t kNs = 1'000'000'000;

struct Cursor {
  std::string_view s;
  std::size_t i = 0;
  bool eof() const { return i >= s.size(); }
  char peek() const { return i < s.size() ? s[i] : '\0'; }
  void skip_ws() {
    while (i < s.size() && (is_space(s[i]) || s[i] == ',')) ++i;
  }
  // Reads up to max_digits digits; returns -1 if none.
  int64_t read_int(int max_digits, int* ndigits = nullptr) {
    int64_t v = 0;
    int n = 0;
    while (i < s.size() && n < max_digits && is_digit(s[i])) {
      v = v * 10 + (s[i] - '0');
      ++i;
      ++n;
    }
    if (ndigits) *ndigits = n;
    return n ? v : -1;
  }
  std::string_view read_alpha() {
    const std::size_t b = i;
    while (i < s.size() && is_alpha(s[i])) ++i;
    return s.substr(b, i - b);
  }
};

int month_from_name(std::string_view n) {
  static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                  "jul", "aug", "sep", "oct", "nov", "dec"};
  if (n.size() < 3) return 0;
  for (int k = 0; k < 12; ++k) {
    if (to_lower(n[0]) == kMonths[k][0] && to_lower(n[1]) == kMonths[k][1] && to_lower(n[2]) == kMonths[k][2])
      return k + 1;
  }
  return 0;
}

// Returns offset in seconds for a zone token, or INT32_MIN if unknown.
int zone_offset_s(std::string_view z) {
  if (z.empty()) return 0;
  if (z[0] == '+' || z[0] == '-') {
    const int sign = z[0] == '-' ? -1 : 1;
    int hh = 0, mm = 0;
    std::string_view r = z.substr(1);
    if (r.size() >= 4 && is_digit(r[0]) && is_digit(r[1])) {
      hh = (r[0] - '0') * 10 + (r[1] - '0');
      std::size_t k = 2;
      if (k < r.size() && r[k] == ':') ++k;
      if (k + 1 < r.size() + 1 && k + 2 <= r.size() && is_digit(r[k]) && is_digit(r[k + 1]))
        mm = (r[k] - '0') * 10 + (r[k + 1] - '0');
      return sign * (hh * 3600 + mm * 60);
    }
    if (r.size() == 2 && is_digit(r[0]) && is_digit(r[1])) return sign * ((r[0] - '0') * 10 + (r[1] - '0')) * 3600;
    return INT32_MIN;
  }
  struct Z {
    const char* n;
    int off;
  };
  static const Z kZones[] = {{"gmt", 0},          {"ut", 0},           {"utc", 0},          {"z", 0},
                             {"est", -5 * 3600},  {"edt", -4 * 3600},  {"cst", -6 * 3600},  {"cdt", -5 * 3600},
                             {"mst", -7 * 3600},  {"mdt", -6 * 3600},  {"pst", -8 * 3600},  {"pdt", -7 * 3600},
                             {"bst", 1 * 3600},   {"cet", 1 * 3600},   {"cest", 2 * 3600}};
  for (const auto& zz : kZones)
    if (iequals(z, zz.n)) return zz.off;
  return INT32_MIN;
}

}  // namespace

std::optional<int64_t> parse_rfc822(std::string_view s) noexcept {
  Cursor c{trim(s)};
  c.skip_ws();
  // Optional day name.
  if (is_alpha(c.peek())) {
    c.read_alpha();
    c.skip_ws();
  }
  const int64_t day = c.read_int(2);
  if (day < 1 || day > 31) return std::nullopt;
  while (c.peek() == ' ' || c.peek() == '-') ++c.i;
  const int mon = month_from_name(c.read_alpha());
  if (!mon) return std::nullopt;
  while (c.peek() == ' ' || c.peek() == '-') ++c.i;
  int ydigits = 0;
  int64_t year = c.read_int(4, &ydigits);
  if (year < 0) return std::nullopt;
  if (ydigits == 2) year += year < 70 ? 2000 : 1900;
  c.skip_ws();
  int64_t hh = 0, mi = 0, ss = 0;
  if (is_digit(c.peek())) {
    hh = c.read_int(2);
    if (c.peek() != ':') return std::nullopt;
    ++c.i;
    mi = c.read_int(2);
    if (mi < 0) return std::nullopt;
    if (c.peek() == ':') {
      ++c.i;
      ss = c.read_int(2);
      if (ss < 0) return std::nullopt;
    }
  }
  c.skip_ws();
  const std::string_view zone = trim(c.s.substr(c.i));
  int off = zone_offset_s(zone);
  if (off == INT32_MIN) off = 0;  // unknown zone: assume UTC
  const int64_t days = days_from_civil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
  const int64_t secs = days * 86400 + hh * 3600 + mi * 60 + ss - off;
  return secs * kNs;
}

std::optional<int64_t> parse_iso8601(std::string_view s) noexcept {
  Cursor c{trim(s)};
  const int64_t y = c.read_int(4);
  if (y < 0 || c.peek() != '-') return std::nullopt;
  ++c.i;
  const int64_t mo = c.read_int(2);
  if (mo < 1 || mo > 12 || c.peek() != '-') return std::nullopt;
  ++c.i;
  const int64_t d = c.read_int(2);
  if (d < 1 || d > 31) return std::nullopt;
  int64_t hh = 0, mi = 0, ss = 0, frac_ns = 0;
  int off = 0;
  if (c.peek() == 'T' || c.peek() == 't' || c.peek() == ' ') {
    ++c.i;
    hh = c.read_int(2);
    if (hh < 0 || c.peek() != ':') return std::nullopt;
    ++c.i;
    mi = c.read_int(2);
    if (mi < 0) return std::nullopt;
    if (c.peek() == ':') {
      ++c.i;
      ss = c.read_int(2);
      if (ss < 0) return std::nullopt;
    }
    if (c.peek() == '.' || c.peek() == ',') {
      ++c.i;
      int nd = 0;
      int64_t f = c.read_int(9, &nd);
      while (is_digit(c.peek())) ++c.i;  // ignore beyond ns
      if (f > 0) {
        for (int k = nd; k < 9; ++k) f *= 10;
        frac_ns = f;
      }
    }
    const std::string_view zone = c.s.substr(c.i);
    if (!zone.empty()) {
      off = zone_offset_s(zone);
      if (off == INT32_MIN) return std::nullopt;
    }
  }
  const int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return (days * 86400 + hh * 3600 + mi * 60 + ss - off) * kNs + frac_ns;
}

std::optional<int64_t> parse_any_datetime(std::string_view s) noexcept {
  s = trim(s);
  if (s.size() >= 10 && is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2]) && is_digit(s[3]) && s[4] == '-')
    return parse_iso8601(s);
  return parse_rfc822(s);
}

int eastern_utc_offset_s(int64_t epoch_s) noexcept {
  const int64_t days = epoch_s >= 0 ? epoch_s / 86400 : (epoch_s - 86399) / 86400;
  int y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  const int64_t mar1 = days_from_civil(y, 3, 1);
  const int64_t nov1 = days_from_civil(y, 11, 1);
  const int64_t first_sun_mar = mar1 + (7 - weekday_from_days(mar1)) % 7;
  const int64_t second_sun_mar = first_sun_mar + 7;
  const int64_t first_sun_nov = nov1 + (7 - weekday_from_days(nov1)) % 7;
  const int64_t dst_start = second_sun_mar * 86400 + 7 * 3600;  // 02:00 EST
  const int64_t dst_end = first_sun_nov * 86400 + 6 * 3600;     // 02:00 EDT
  return (epoch_s >= dst_start && epoch_s < dst_end) ? -4 * 3600 : -5 * 3600;
}

int64_t eastern_midnight_ns(int y, unsigned m, unsigned d) noexcept {
  const int64_t days = days_from_civil(y, m, d);
  const int off = eastern_utc_offset_s(days * 86400 + 5 * 3600);
  return (days * 86400 - off) * kNs;
}

void eastern_date(int64_t epoch_ns, int& y, unsigned& m, unsigned& d) noexcept {
  const int64_t s = epoch_ns / kNs;
  const int64_t local = s + eastern_utc_offset_s(s);
  const int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
  civil_from_days(days, y, m, d);
}

int eastern_minute_of_day(int64_t epoch_ns) noexcept {
  const int64_t s = epoch_ns / kNs;
  int64_t local = s + eastern_utc_offset_s(s);
  local %= 86400;
  if (local < 0) local += 86400;
  return static_cast<int>(local / 60);
}

std::optional<int64_t> parse_eastern_mdy_time(std::string_view mdy, std::string_view hms) noexcept {
  Cursor c{trim(mdy)};
  const int64_t mo = c.read_int(2);
  if (mo < 1 || mo > 12 || c.peek() != '/') return std::nullopt;
  ++c.i;
  const int64_t d = c.read_int(2);
  if (d < 1 || d > 31 || c.peek() != '/') return std::nullopt;
  ++c.i;
  const int64_t y = c.read_int(4);
  if (y < 1900) return std::nullopt;
  Cursor t{trim(hms)};
  int64_t hh = 0, mi = 0, ss = 0, ms = 0;
  if (!t.eof()) {
    hh = t.read_int(2);
    if (hh < 0 || t.peek() != ':') return std::nullopt;
    ++t.i;
    mi = t.read_int(2);
    if (mi < 0) return std::nullopt;
    if (t.peek() == ':') {
      ++t.i;
      ss = t.read_int(2);
      if (ss < 0) return std::nullopt;
    }
    if (t.peek() == '.') {
      ++t.i;
      int nd = 0;
      ms = t.read_int(3, &nd);
      if (ms < 0) ms = 0;
      for (int k = nd; k < 3; ++k) ms *= 10;
    }
  }
  return eastern_midnight_ns(static_cast<int>(y), static_cast<unsigned>(mo), static_cast<unsigned>(d)) +
         (hh * 3600 + mi * 60 + ss) * kNs + ms * 1'000'000;
}

int format_utc_iso(int64_t epoch_ns, char* buf, std::size_t cap) noexcept {
  const int64_t s = epoch_ns >= 0 ? epoch_ns / kNs : (epoch_ns - kNs + 1) / kNs;
  const int64_t ms = (epoch_ns - s * kNs) / 1'000'000;
  const int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
  const int64_t sod = s - days * 86400;
  int y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  return std::snprintf(buf, cap, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, m, d, static_cast<int>(sod / 3600),
                       static_cast<int>((sod / 60) % 60), static_cast<int>(sod % 60), static_cast<int>(ms));
}

std::string format_utc_iso(int64_t epoch_ns) {
  char buf[40];
  const int n = format_utc_iso(epoch_ns, buf, sizeof(buf));
  return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
}

std::string eastern_date_str(int64_t epoch_ns) {
  int y;
  unsigned m, d;
  eastern_date(epoch_ns, y, m, d);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
  return buf;
}

std::string eastern_hms(int64_t epoch_ns) {
  const int64_t s = epoch_ns / kNs;
  int64_t local = (s + eastern_utc_offset_s(s)) % 86400;
  if (local < 0) local += 86400;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", static_cast<int>(local / 3600),
                static_cast<int>((local / 60) % 60), static_cast<int>(local % 60));
  return buf;
}

bool parse_ymd(std::string_view s, int& y, unsigned& m, unsigned& d) noexcept {
  s = trim(s);
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
  auto yy = parse_int<int>(s.substr(0, 4));
  auto mm = parse_int<unsigned>(s.substr(5, 2));
  auto dd = parse_int<unsigned>(s.substr(8, 2));
  if (!yy || !mm || !dd || *mm < 1 || *mm > 12 || *dd < 1 || *dd > 31) return false;
  y = *yy;
  m = *mm;
  d = *dd;
  return true;
}

}  // namespace stok::timeutil
