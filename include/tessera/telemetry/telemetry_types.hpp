#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>

namespace tessera {

enum class MetricType : uint8_t {
  COUNTER = 0,
  GAUGE = 1,
  HISTOGRAM = 2
};

struct MetricLabel {
  std::string key;
  std::string value;

  bool operator==(const MetricLabel& other) const noexcept {
    return key == other.key && value == other.value;
  }
};

struct HistogramSnapshot {
  std::vector<double> bucket_bounds;
  std::vector<uint64_t> bucket_counts;
  uint64_t total_count{0};
  double sum{0.0};
};

class Histogram {
 public:
  explicit Histogram(std::vector<double> bucket_bounds = {1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0})
      : bucket_bounds_(std::move(bucket_bounds)),
        bucket_counts_(bucket_bounds_.size()),
        total_count_(0),
        sum_micros_(0) {
    for (auto& count : bucket_counts_) {
      count.store(0, std::memory_order_relaxed);
    }
  }

  // Non-copyable due to atomics, but moveable via custom constructor
  Histogram(Histogram&& other) noexcept
      : bucket_bounds_(std::move(other.bucket_bounds_)),
        bucket_counts_(other.bucket_bounds_.size()),
        total_count_(other.total_count_.load(std::memory_order_relaxed)),
        sum_micros_(other.sum_micros_.load(std::memory_order_relaxed)) {
    for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
      bucket_counts_[i].store(other.bucket_counts_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }

  void Observe(double val_ms) {
    if (val_ms < 0.0) val_ms = 0.0;
    total_count_.fetch_add(1, std::memory_order_relaxed);
    sum_micros_.fetch_add(static_cast<uint64_t>(val_ms * 1000.0), std::memory_order_relaxed);

    for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
      if (val_ms <= bucket_bounds_[i]) {
        bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] HistogramSnapshot Snapshot() const {
    HistogramSnapshot snap;
    snap.bucket_bounds = bucket_bounds_;
    snap.bucket_counts.resize(bucket_bounds_.size());
    for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
      snap.bucket_counts[i] = bucket_counts_[i].load(std::memory_order_relaxed);
    }
    snap.total_count = total_count_.load(std::memory_order_relaxed);
    snap.sum = static_cast<double>(sum_micros_.load(std::memory_order_relaxed)) / 1000.0;
    return snap;
  }

  void Reset() {
    for (auto& count : bucket_counts_) {
      count.store(0, std::memory_order_relaxed);
    }
    total_count_.store(0, std::memory_order_relaxed);
    sum_micros_.store(0, std::memory_order_relaxed);
  }

 private:
  std::vector<double> bucket_bounds_;
  std::vector<std::atomic<uint64_t>> bucket_counts_;
  std::atomic<uint64_t> total_count_{0};
  std::atomic<uint64_t> sum_micros_{0};
};

}  // namespace tessera

