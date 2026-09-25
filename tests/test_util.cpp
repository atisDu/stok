#include "check.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

using namespace stok;
using namespace stok::timeutil;

namespace {
constexpr int64_t kS = 1'000'000'000;
}

TEST(time_rfc822_variants) {
  // 2026-09-25 12:00:00 UTC = 1790337600
  const int64_t base = 1790337600LL * kS;
  CHECK_EQ(parse_rfc822("Fri, 25 Sep 2026 12:00:00 GMT").value_or(0), base);
  CHECK_EQ(parse_rfc822("Fri, 25 Sep 2026 12:00 GMT").value_or(0), base);
  CHECK_EQ(parse_rfc822("25 Sep 2026 08:00:00 -0400").value_or(0), base);
  CHECK_EQ(parse_rfc822("Fri, 25 Sep 2026 08:00:00 EDT").value_or(0), base);
  CHECK_EQ(parse_rfc822("Fri, 25 Sep 2026 12:00:00 +0000").value_or(0), base);
  CHECK(!parse_rfc822("garbage").has_value());
}

TEST(time_iso8601_variants) {
  const int64_t base = 1790337600LL * kS;
  CHECK_EQ(parse_iso8601("2026-09-25T08:00:00-04:00").value_or(0), base);
  CHECK_EQ(parse_iso8601("2026-09-25T12:00:00Z").value_or(0), base);
  CHECK_EQ(parse_iso8601("2026-09-25T12:00:00.250Z").value_or(0), base + 250'000'000);
  CHECK_EQ(parse_iso8601("2026-09-25T12:00:00").value_or(0), base);
  CHECK_EQ(parse_any_datetime("2026-09-25T08:00:00-04:00").value_or(0), base);
  CHECK_EQ(parse_any_datetime("Fri, 25 Sep 2026 12:00:00 GMT").value_or(0), base);
}

TEST(time_eastern_dst_rules) {
  // 2026: DST from Sun Mar 8 07:00 UTC to Sun Nov 1 06:00 UTC.
  const int64_t mar8_0659 = (days_from_civil(2026, 3, 8) * 86400 + 6 * 3600 + 59 * 60);
  const int64_t mar8_0700 = (days_from_civil(2026, 3, 8) * 86400 + 7 * 3600);
  CHECK_EQ(eastern_utc_offset_s(mar8_0659), -5 * 3600);
  CHECK_EQ(eastern_utc_offset_s(mar8_0700), -4 * 3600);
  const int64_t nov1_0559 = (days_from_civil(2026, 11, 1) * 86400 + 5 * 3600 + 59 * 60);
  const int64_t nov1_0600 = (days_from_civil(2026, 11, 1) * 86400 + 6 * 3600);
  CHECK_EQ(eastern_utc_offset_s(nov1_0559), -4 * 3600);
  CHECK_EQ(eastern_utc_offset_s(nov1_0600), -5 * 3600);
  // Midnight ET on a summer and a winter day.
  CHECK_EQ(eastern_midnight_ns(2026, 9, 25), (days_from_civil(2026, 9, 25) * 86400 + 4 * 3600) * kS);
  CHECK_EQ(eastern_midnight_ns(2026, 12, 1), (days_from_civil(2026, 12, 1) * 86400 + 5 * 3600) * kS);
  // 09:30 ET on 2026-09-25 = 13:30 UTC
  const int64_t open = (days_from_civil(2026, 9, 25) * 86400 + 13 * 3600 + 30 * 60) * kS;
  CHECK_EQ(eastern_minute_of_day(open), 9 * 60 + 30);
  CHECK_EQ(eastern_date_str(open), std::string("2026-09-25"));
  CHECK_EQ(eastern_hms(open), std::string("09:30:00"));
  CHECK_EQ(parse_eastern_mdy_time("09/25/2026", "09:30:00").value_or(0), open);
  CHECK_EQ(parse_eastern_mdy_time("09/25/2026", "09:30:00.500").value_or(0), open + 500'000'000);
  CHECK_EQ(format_utc_iso(open), std::string("2026-09-25T13:30:00.000Z"));
}

TEST(text_entities_and_html) {
  std::string out;
  text::append_decoded_entities("AT&amp;T &lt;b&gt; &#8217;s &#x2014; &nbsp;&unknown; &", out);
  CHECK_EQ(out, std::string("AT&T <b> \xE2\x80\x99s \xE2\x80\x94 \xC2\xA0&unknown; &"));
  std::string t;
  text::html_to_text("<p>Hello&nbsp;<b>world</b></p><script>var x=1;</script><p>Next &amp; last</p><!-- c -->", t);
  CHECK_EQ(t, std::string("Hello world Next & last"));
  std::string lines;
  text::html_to_text("<p>Headline</p><p>Body text</p>", lines, SIZE_MAX, '\n');
  CHECK_EQ(lines, std::string("Headline\nBody text"));
}

TEST(text_headline_key_normalizes) {
  CHECK_EQ(text::headline_key("Acme Signs $20 Million Deal!"), text::headline_key("ACME signs $20 million deal"));
  CHECK_EQ(text::headline_key("Acme\xE2\x80\x99s Deal"), text::headline_key("Acme's  Deal"));
  CHECK(text::headline_key("Acme Signs Deal") != text::headline_key("Acme Ends Deal"));
}

TEST(json_parse_and_unescape) {
  JsonDoc d;
  const std::string src = R"({"fields":["cik","name"],"data":[[320193,"Apple \"Inc\" é"],[1,"x"]],"t":true,"n":null,"f":-1.5e3})";
  CHECK(d.parse(src));
  const auto* data = d.get(d.root(), "data");
  CHECK(data && data->count == 2);
  const auto* row0 = d.at(*data, 0);
  CHECK(row0 != nullptr);
  if (row0) {
    CHECK_EQ(JsonDoc::num(d.at(*row0, 0)), 320193.0);
    CHECK_EQ(JsonDoc::str(d.at(*row0, 1)), std::string("Apple \"Inc\" \xC3\xA9"));
  }
  CHECK_EQ(JsonDoc::num(d.get(d.root(), "f")), -1500.0);
  JsonDoc bad;
  CHECK(!bad.parse("{\"a\":[1,2}"));
}
