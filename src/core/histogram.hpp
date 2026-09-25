#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace stok {

// Log-linear latency histogram (HdrHistogram-style, ~6% relative precision),
// fixed memory and O(1) record. Single-writer; the owning thread reports it.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 4;  // 16 linear sub-buckets per power of two
  static constexpr int kBuckets = 64 << kSubBits;

  void record(uint64_t v) noexcept {
    ++counts_[index(v)];
    ++n_;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
    sum_ += v;
  }

  uint64_t count() const noexcept { return n_; }
  uint64_t max() const noexcept { return n_ ? max_ : 0; }
  uint64_t min() const noexcept { return n_ ? min_ : 0; }
  double mean() const noexcept { return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0; }

  // Upper bound of the bucket containing the p-th percentile (0..100).
  uint64_t percentile(double p) const noexcept {
    if (n_ == 0) return 0;
    const uint64_t target = static_cast<uint64_t>(std::max(1.0, p / 100.0 * static_cast<double>(n_) + 0.5));
    uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
      acc += counts_[i];
      if (acc >= target) return std::min(upper_bound(i), max_);
    }
    return max_;
  }

  void reset() noexcept {
    counts_.fill(0);
    n_ = 0;
    max_ = 0;
    min_ = ~0ull;
    sum_ = 0;
  }

  void merge(const LatencyHistogram& o) noexcept {
    for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    n_ += o.n_;
    sum_ += o.sum_;
    max_ = std::max(max_, o.max_);
    min_ = std::min(min_, o.min_);
  }

 private:
  static int index(uint64_t v) noexcept {
    if (v < (1u << kSubBits)) return static_cast<int>(v);
    const int msb = 63 - __builtin_clzll(v);
    const int shift = msb - kSubBits;
    const int sub = static_cast<int>((v >> shift) & ((1u << kSubBits) - 1));
    return ((shift + 1) << kSubBits) + sub;
  }
  static uint64_t upper_bound(int idx) noexcept {
    if (idx < (1 << kSubBits)) return static_cast<uint64_t>(idx);
    const int shift = (idx >> kSubBits) - 1;
    const uint64_t sub = static_cast<uint64_t>(idx & ((1 << kSubBits) - 1));
    return (((1ull << kSubBits) | sub) << shift) + ((1ull << shift) - 1);
  }

  std::array<uint64_t, kBuckets> counts_{};
  uint64_t n_ = 0;
  uint64_t max_ = 0;
  uint64_t min_ = ~0ull;
  uint64_t sum_ = 0;
};

}  // namespace stok
