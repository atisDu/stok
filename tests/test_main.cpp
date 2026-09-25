#include <cstring>

#include "check.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  stok::Logger::instance().set_level(stok::LogLevel::Warn);
  int run = 0;
  for (const auto& c : stok::test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = stok::test::failures();
    const uint64_t t0 = stok::mono_ns();
    c.fn();
    const double ms = static_cast<double>(stok::mono_ns() - t0) / 1e6;
    std::printf("%s %-44s %8.2f ms\n", stok::test::failures() == before ? "ok  " : "FAIL", c.name, ms);
    ++run;
  }
  std::printf("\n%d tests, %d failed checks\n", run, stok::test::failures());
  stok::Logger::instance().stop();
  return stok::test::failures() ? 1 : 0;
}
