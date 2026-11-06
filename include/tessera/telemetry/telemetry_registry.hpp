#pragma once

#include "tessera/routing/circuit_types.hpp"
#include "tessera/telemetry/telemetry_types.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace tessera {

class TelemetryRegistry {
 public:
  static TelemetryRegistry& Instance();

  // Metrics collection API (< 1 microsecond overhead, thread-safe)
  void IncrementRequestsTotal(
      std::string_view model,
      std::string_view provider,
      std::string_view status_code);

  void IncrementCacheHitsTotal(std::string_view tier); // "exact", "semantic", "miss"

  void SetCircuitBreakerState(std::string_view provider, CircuitState state);

  void RecordUpstreamLatency(std::string_view provider, double latency_ms);

  // Serializes all captured metrics into standard Prometheus text format
  [[nodiscard]] std::string ToPrometheusFormat() const;

  // Test inspection hooks
  [[nodiscard]] uint64_t GetRequestsTotal(std::string_view model, std::string_view provider, std::string_view status_code) const;
  [[nodiscard]] uint64_t GetCacheHitsTotal(std::string_view tier) const;
  [[nodiscard]] uint32_t GetCircuitBreakerState(std::string_view provider) const;
  [[nodiscard]] HistogramSnapshot GetUpstreamLatencySnapshot(std::string_view provider) const;

  // Resets all metrics (for hermetic unit testing)
  void Reset();

 private:
  TelemetryRegistry();
  ~TelemetryRegistry() = default;

  mutable std::mutex mutex_;

  // Requests counter map: key = "model:provider:status" -> atomic counter
  std::map<std::string, std::shared_ptr<std::atomic<uint64_t>>> requests_counters_;

  // Cache hits counter map: key = tier ("exact", "semantic", "miss") -> atomic counter
  std::atomic<uint64_t> exact_cache_hits_{0};
  std::atomic<uint64_t> semantic_cache_hits_{0};
  std::atomic<uint64_t> cache_misses_{0};

  // Circuit breaker state gauges: provider -> atomic uint32_t
  std::map<std::string, std::shared_ptr<std::atomic<uint32_t>>> circuit_gauges_;

  // Upstream latency histograms: provider -> Histogram
  std::map<std::string, std::shared_ptr<Histogram>> latency_histograms_;
};

}  // namespace tessera

