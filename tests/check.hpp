#pragma once

// Minimal self-contained test harness (no external dependency).

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "util/file.hpp"

namespace stok::test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

struct Reg {
  Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
}

template <typename T>
std::string show(const T& v) {
  std::ostringstream ss;
  ss << v;
  return ss.str();
}

inline std::string fixture(const std::string& name) {
#ifdef STOK_SOURCE_DIR
  const std::string base = std::string(STOK_SOURCE_DIR) + "/tests/fixtures/";
#else
  const std::string base = "tests/fixtures/";
#endif
  auto t = fileutil::read_file(base + name);
  if (!t) {
    fail(__FILE__, __LINE__, "missing fixture " + name);
    return {};
  }
  return *t;
}

inline std::string source_path(const std::string& rel) {
#ifdef STOK_SOURCE_DIR
  return std::string(STOK_SOURCE_DIR) + "/" + rel;
#else
  return rel;
#endif
}

}  // namespace stok::test

#define TEST(name)                                                   \
  static void name();                                                \
  static ::stok::test::Reg reg_##name(#name, &name);                 \
  static void name()

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) ::stok::test::fail(__FILE__, __LINE__, #cond);      \
  } while (0)

#define CHECK_EQ(a, b)                                                                                   \
  do {                                                                                                   \
    const auto va_ = (a);                                                                               \
    const auto vb_ = (b);                                                                               \
    if (!(va_ == vb_))                                                                                   \
      ::stok::test::fail(__FILE__, __LINE__,                                                             \
                         std::string(#a " == " #b " (") + ::stok::test::show(va_) + " vs " +           \
                             ::stok::test::show(vb_) + ")");                                             \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                            \
  do {                                                                                                   \
    const double va_ = (a), vb_ = (b);                                                                   \
    if (!(std::fabs(va_ - vb_) <= (eps)))                                                                \
      ::stok::test::fail(__FILE__, __LINE__,                                                             \
                         std::string(#a " ~= " #b " (") + ::stok::test::show(va_) + " vs " +           \
                             ::stok::test::show(vb_) + ")");                                             \
  } while (0)
