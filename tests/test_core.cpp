#include <random>
#include <thread>
#include <unordered_map>

#include "check.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/fixed_string.hpp"
#include "core/flat_hash.hpp"
#include "core/hash.hpp"
#include "core/histogram.hpp"
#include "core/mpsc_queue.hpp"
#include "core/seqlock.hpp"
#include "core/spsc_queue.hpp"
#include "core/waker.hpp"

using namespace stok;

TEST(spsc_basic_wraparound) {
  SpscQueue<int> q(4);
  CHECK_EQ(q.capacity(), 4u);
  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(round * 10 + i));
    CHECK(!q.try_push(99));  // full
    for (int i = 0; i < 4; ++i) {
      int* v = q.front();
      CHECK(v != nullptr);
      if (v) CHECK_EQ(*v, round * 10 + i);
      q.pop();
    }
    CHECK(q.front() == nullptr);
  }
}

TEST(spsc_two_threads_preserve_order) {
  SpscQueue<uint64_t> q(1024);
  constexpr uint64_t kN = 2'000'000;
  std::thread prod([&] {
    for (uint64_t i = 1; i <= kN; ++i) {
      uint64_t* s;
      while (!(s = q.try_claim())) cpu_relax();
      *s = i;
      q.publish();
    }
  });
  uint64_t expect = 1;
  bool ordered = true;
  while (expect <= kN) {
    if (uint64_t* v = q.front()) {
      ordered &= *v == expect;
      ++expect;
      q.pop();
    } else {
      cpu_relax();
    }
  }
  prod.join();
  CHECK(ordered);
}

TEST(mpsc_multiple_producers) {
  MpscQueue<uint64_t> q(4096);
  constexpr int kThreads = 4;
  constexpr uint64_t kPer = 200'000;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (uint64_t i = 0; i < kPer; ++i)
        while (!q.try_push_with([&](uint64_t& s) { s = (static_cast<uint64_t>(t) << 32) | i; })) cpu_relax();
    });
  }
  std::vector<uint64_t> next(kThreads, 0);
  uint64_t got = 0;
  bool ordered = true;
  while (got < kThreads * kPer) {
    if (uint64_t* v = q.front()) {
      const int t = static_cast<int>(*v >> 32);
      ordered &= (*v & 0xffffffffu) == next[static_cast<std::size_t>(t)];
      ++next[static_cast<std::size_t>(t)];
      q.pop();
      ++got;
    } else {
      cpu_relax();
    }
  }
  for (auto& th : ts) th.join();
  CHECK(ordered);  // per-producer FIFO
}

TEST(flat_map_matches_unordered_map) {
  FlatMap64<uint64_t> m(8);
  std::unordered_map<uint64_t, uint64_t> ref;
  std::mt19937_64 rng(42);
  for (int i = 0; i < 200'000; ++i) {
    const uint64_t k = rng() % 5000;
    const int op = static_cast<int>(rng() % 3);
    if (op == 0) {
      m.insert_or_assign(k, i);
      ref[k] = static_cast<uint64_t>(i);
    } else if (op == 1) {
      CHECK_EQ(m.erase(k), ref.erase(k) == 1);
    } else {
      const uint64_t* v = m.find(k);
      auto it = ref.find(k);
      CHECK_EQ(v != nullptr, it != ref.end());
      if (v && it != ref.end()) CHECK_EQ(*v, it->second);
    }
  }
  CHECK_EQ(m.size(), ref.size());
  std::size_t n = 0;
  m.for_each([&](uint64_t k, uint64_t v) {
    ++n;
    CHECK_EQ(ref.at(k), v);
  });
  CHECK_EQ(n, ref.size());
}

TEST(seqlock_readers_never_see_torn_state) {
  struct Pair {
    uint64_t a, b, c, d;
  };
  SeqLocked<Pair> sl;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
      Pair& p = sl.begin_write();
      p.a = i;
      p.b = i * 2;
      p.c = i * 3;
      p.d = i * 4;
      sl.end_write();
    }
  });
  bool torn = false;
  for (int i = 0; i < 500'000; ++i) {
    const Pair p = sl.load();
    torn |= p.b != p.a * 2 || p.c != p.a * 3 || p.d != p.a * 4;
  }
  stop = true;
  writer.join();
  CHECK(!torn);
}

TEST(histogram_percentiles) {
  LatencyHistogram h;
  for (uint64_t v = 1; v <= 10000; ++v) h.record(v * 1000);
  CHECK_EQ(h.count(), 10000u);
  CHECK_NEAR(static_cast<double>(h.percentile(50)), 5'000'000.0, 5'000'000.0 * 0.07);
  CHECK_NEAR(static_cast<double>(h.percentile(99)), 9'900'000.0, 9'900'000.0 * 0.07);
  CHECK_EQ(h.max(), 10'000'000u);
  CHECK_EQ(h.min(), 1000u);
}

TEST(fixed_string_truncates_on_utf8_boundary) {
  FixedStr<8> s;
  s.assign("abc\xE2\x80\x94xyz");  // 9 bytes into capacity 7: "abc" + em dash + "x"
  CHECK_EQ(s.len, 7);
  s.assign("abcde\xE2\x80\x94");   // 5 + 3 = 8 bytes; capacity 7 -> must drop the dash entirely
  CHECK_EQ(std::string(s.view()), std::string("abcde"));
}

TEST(hash_is_stable) {
  // Journal ids must be comparable across runs and machines.
  CHECK_EQ(hash_sv("hello"), hash_sv("hello"));
  CHECK(hash_sv("hello") != hash_sv("hellp"));
  CHECK(hash_sv("") != hash_sv(" "));
  std::string big(1000, 'x');
  const uint64_t h1 = hash_sv(big);
  big[999] = 'y';
  CHECK(h1 != hash_sv(big));
}

TEST(config_parse_sections_and_comments) {
  const char* text =
      "# comment\n"
      "[general]\n"
      "data_dir = data   # trailing comment\n"
      "[feed edgar_8k]\n"
      "url = https://example.com/a?b=c&d=e#frag\n"
      "interval_ms = 500\n"
      "[feed wire]\n"
      "url = \"https://x.test/rss\"\n"
      "enabled = no\n"
      "[signal]\n"
      "high_max_shares = 30e6\n";
  std::string err;
  Config c = Config::parse(text, &err);
  CHECK(err.empty());
  CHECK_EQ(c.str("general", "data_dir"), std::string("data"));
  auto feeds = c.sections("feed");
  CHECK_EQ(feeds.size(), 2u);
  CHECK_EQ(feeds[0]->arg, std::string("edgar_8k"));
  CHECK_EQ(feeds[0]->get_str("url"), std::string("https://example.com/a?b=c&d=e#frag"));
  CHECK_EQ(feeds[0]->get_int("interval_ms", 0), 500);
  CHECK_EQ(feeds[1]->get_str("url"), std::string("https://x.test/rss"));
  CHECK_EQ(feeds[1]->get_bool("enabled", true), false);
  CHECK_EQ(c.integer("signal", "high_max_shares", 0), 30'000'000);
}

TEST(waker_wakes_sleeper) {
  Waker w;
  std::atomic<bool> flag{false};
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    flag = true;
    w.notify();
  });
  const uint64_t t0 = mono_ns();
  while (!flag.load()) w.wait(5 * kNsPerSec, [&] { return flag.load(); });
  const uint64_t waited = mono_ns() - t0;
  t.join();
  CHECK(waited < 2 * kNsPerSec);  // woke on notify, not the 5 s timeout
}
