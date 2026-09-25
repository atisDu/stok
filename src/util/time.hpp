#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Date/time helpers with no dependency on the system time-zone database. US
// Eastern DST rules (in force since 2007) are computed directly, so the daemon
// behaves the same in any container.
namespace stok::timeutil {

constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) noexcept;

// 0 = Sunday.
constexpr int weekday_from_days(int64_t z) noexcept {
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

// Parses RFC 822/1123 dates used by RSS: "Thu, 25 Sep 2026 12:00:00 GMT",
// "25 Sep 2026 08:00 -0400", "... EDT". Returns epoch ns.
std::optional<int64_t> parse_rfc822(std::string_view s) noexcept;

// Parses ISO 8601 / RFC 3339: "2026-09-25T10:14:32-04:00", "...Z",
// fractional seconds, or a bare date. No zone means UTC. Returns epoch ns.
std::optional<int64_t> parse_iso8601(std::string_view s) noexcept;

// Either format.
std::optional<int64_t> parse_any_datetime(std::string_view s) noexcept;

// US Eastern UTC offset in seconds (-14400 EDT or -18000 EST) at an instant.
int eastern_utc_offset_s(int64_t epoch_s) noexcept;

// Epoch ns of 00:00 America/New_York on the given civil date.
int64_t eastern_midnight_ns(int y, unsigned m, unsigned d) noexcept;

// US Eastern civil date of an instant.
void eastern_date(int64_t epoch_ns, int& y, unsigned& m, unsigned& d) noexcept;

// Minutes since 00:00 US Eastern (0..1439) of an instant.
int eastern_minute_of_day(int64_t epoch_ns) noexcept;

// "MM/DD/YYYY" + "HH:MM:SS[.fff]" interpreted as US Eastern -> epoch ns.
std::optional<int64_t> parse_eastern_mdy_time(std::string_view mdy, std::string_view hms) noexcept;

// "2026-09-25T14:05:12.123Z"
std::string format_utc_iso(int64_t epoch_ns);
// Writes the same into buf (at least 25 bytes). Returns length.
int format_utc_iso(int64_t epoch_ns, char* buf, std::size_t cap) noexcept;
// "2026-09-25" (US Eastern date)
std::string eastern_date_str(int64_t epoch_ns);
// "09:31:05" US Eastern clock time.
std::string eastern_hms(int64_t epoch_ns);

// Parses "YYYY-MM-DD".
bool parse_ymd(std::string_view s, int& y, unsigned& m, unsigned& d) noexcept;

}  // namespace stok::timeutil
