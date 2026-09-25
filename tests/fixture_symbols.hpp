#pragma once

#include "check.hpp"
#include "ref/symbols.hpp"

namespace stok::test {

// Security master from the fixture files (Nasdaq Trader + SEC map), plus
// fundamentals and a baseline so universe/materiality logic has inputs.
inline SymbolTable fixture_symbols() {
  SymbolTable t;
  t.load_nasdaq_listed(fixture("nasdaqlisted.txt"));
  t.load_other_listed(fixture("otherlisted.txt"));
  t.load_sec_tickers(fixture("company_tickers_exchange.json"));
  t.load_fundamentals(
      "cik\tshares_outstanding\tshares_asof\tpublic_float_usd\tfloat_asof\n"
      "1234567\t12000000\t2026-08-01\t20000000\t2026-06-30\n"
      "7654321\t40000000\t2026-08-01\t0\t\n"
      "2222222\t25000000\t2026-08-01\t0\t\n"
      "3333333\t900000000\t2026-08-01\t0\t\n");
  t.load_baseline(
      "ticker\tprev_close\tadv\tdays\tasof\n"
      "ACMR\t2.00\t500000\t20\t2026-09-24\n"
      "BRVO\t2.50\t800000\t20\t2026-09-24\n"
      "DLTX\t4.00\t300000\t20\t2026-09-24\n"
      "CHLY\t45.00\t2000000\t20\t2026-09-24\n");
  t.build_name_index();
  return t;
}

}  // namespace stok::test
