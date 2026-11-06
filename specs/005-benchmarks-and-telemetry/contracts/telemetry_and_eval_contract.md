# Interface & Metrics Contracts: Observability & Evaluation

**Feature Branch**: `005-benchmarks-and-telemetry`  
**Date**: 2026-11-06  
**Status**: Completed  

---

## 1. C++ Observability Contract: `TelemetryRegistry`

```cpp
#pragma once

#include "tessera/routing/circuit_types.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

  // Resets all metrics (for hermetic unit testing)
  void Reset();

 private:
  TelemetryRegistry();
  ~TelemetryRegistry() = default;
};

}  // namespace tessera
```

---

## 2. Prometheus Exposition Format Contract

The output of `ToPrometheusFormat()` conforms to Prometheus text exposition format version 0.0.4:

```text
# HELP gateway_requests_total Total number of chat completion requests processed by the gateway
# TYPE gateway_requests_total counter
gateway_requests_total{model="llama-3.3-70b-versatile",provider="groq",status="200"} 1245
gateway_requests_total{model="google/gemma-2-9b-it",provider="openrouter",status="200"} 84
gateway_requests_total{model="llama-3.3-70b-versatile",provider="groq",status="429"} 3

# HELP gateway_cache_hits_total Total number of completion requests resolved from cache tiers
# TYPE gateway_cache_hits_total counter
gateway_cache_hits_total{tier="exact"} 432
gateway_cache_hits_total{tier="semantic"} 289
gateway_cache_hits_total{tier="miss"} 607

# HELP circuit_breaker_state Active state of upstream provider circuit breaker (0=CLOSED, 1=OPEN, 2=HALF-OPEN)
# TYPE circuit_breaker_state gauge
circuit_breaker_state{provider="groq"} 0
circuit_breaker_state{provider="openrouter"} 0

# HELP upstream_latency_ms Latency of upstream LLM completions in milliseconds
# TYPE upstream_latency_ms histogram
upstream_latency_ms_bucket{provider="groq",le="1"} 0
upstream_latency_ms_bucket{provider="groq",le="5"} 2
upstream_latency_ms_bucket{provider="groq",le="20"} 15
upstream_latency_ms_bucket{provider="groq",le="100"} 340
upstream_latency_ms_bucket{provider="groq",le="500"} 980
upstream_latency_ms_bucket{provider="groq",le="+Inf"} 1000
upstream_latency_ms_sum{provider="groq"} 142300.5
upstream_latency_ms_count{provider="groq"} 1000
```

---

## 3. Evaluation Runner Contract: `EvaluationRunner`

```cpp
#pragma once

#include "tessera/backend/completion_backend.hpp"
#include <string>
#include <vector>

namespace tessera {

struct EvaluationConfig {
  size_t query_count = 1000;
  double paraphrase_ratio = 0.30;
  size_t concurrency = 8;
  bool verbose = false;
  std::string output_json_path = "";
};

class EvaluationRunner {
 public:
  explicit EvaluationRunner(std::shared_ptr<ICompletionBackend> backend);

  [[nodiscard]] EvaluationReport Run(const EvaluationConfig& config);

  // Formats human-readable markdown summary
  [[nodiscard]] static std::string FormatMarkdownReport(const EvaluationReport& report);

  // Formats machine-readable JSON for CI regression gating
  [[nodiscard]] static std::string FormatJsonReport(const EvaluationReport& report);
};

}  // namespace tessera
```

