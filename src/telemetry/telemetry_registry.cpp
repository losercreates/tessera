#include "tessera/telemetry/telemetry_registry.hpp"
#include <sstream>

namespace tessera {

TelemetryRegistry& TelemetryRegistry::Instance() {
  static TelemetryRegistry instance;
  return instance;
}

TelemetryRegistry::TelemetryRegistry() {
  // Pre-seed known circuit states
  circuit_gauges_["groq"] = std::make_shared<std::atomic<uint32_t>>(0);
  circuit_gauges_["openrouter"] = std::make_shared<std::atomic<uint32_t>>(0);

  // Pre-seed latency histograms
  latency_histograms_["groq"] = std::make_shared<Histogram>();
  latency_histograms_["openrouter"] = std::make_shared<Histogram>();
}

void TelemetryRegistry::IncrementRequestsTotal(
    std::string_view model,
    std::string_view provider,
    std::string_view status_code) {
  std::string key = std::string(model) + "|" + std::string(provider) + "|" + std::string(status_code);

  std::shared_ptr<std::atomic<uint64_t>> counter;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = requests_counters_.find(key);
    if (it != requests_counters_.end()) {
      counter = it->second;
    } else {
      counter = std::make_shared<std::atomic<uint64_t>>(0);
      requests_counters_[key] = counter;
    }
  }
  counter->fetch_add(1, std::memory_order_relaxed);
}

void TelemetryRegistry::IncrementCacheHitsTotal(std::string_view tier) {
  if (tier == "exact") {
    exact_cache_hits_.fetch_add(1, std::memory_order_relaxed);
  } else if (tier == "semantic") {
    semantic_cache_hits_.fetch_add(1, std::memory_order_relaxed);
  } else {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
  }
}

void TelemetryRegistry::SetCircuitBreakerState(std::string_view provider, CircuitState state) {
  std::string prov(provider);
  std::shared_ptr<std::atomic<uint32_t>> gauge;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = circuit_gauges_.find(prov);
    if (it != circuit_gauges_.end()) {
      gauge = it->second;
    } else {
      gauge = std::make_shared<std::atomic<uint32_t>>(0);
      circuit_gauges_[prov] = gauge;
    }
  }
  gauge->store(static_cast<uint32_t>(state), std::memory_order_relaxed);
}

void TelemetryRegistry::RecordUpstreamLatency(std::string_view provider, double latency_ms) {
  std::string prov(provider);
  std::shared_ptr<Histogram> hist;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latency_histograms_.find(prov);
    if (it != latency_histograms_.end()) {
      hist = it->second;
    } else {
      hist = std::make_shared<Histogram>();
      latency_histograms_[prov] = hist;
    }
  }
  hist->Observe(latency_ms);
}

uint64_t TelemetryRegistry::GetRequestsTotal(
    std::string_view model,
    std::string_view provider,
    std::string_view status_code) const {
  std::string key = std::string(model) + "|" + std::string(provider) + "|" + std::string(status_code);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = requests_counters_.find(key);
  if (it != requests_counters_.end()) {
    return it->second->load(std::memory_order_relaxed);
  }
  return 0;
}

uint64_t TelemetryRegistry::GetCacheHitsTotal(std::string_view tier) const {
  if (tier == "exact") return exact_cache_hits_.load(std::memory_order_relaxed);
  if (tier == "semantic") return semantic_cache_hits_.load(std::memory_order_relaxed);
  return cache_misses_.load(std::memory_order_relaxed);
}

uint32_t TelemetryRegistry::GetCircuitBreakerState(std::string_view provider) const {
  std::string prov(provider);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = circuit_gauges_.find(prov);
  if (it != circuit_gauges_.end()) {
    return it->second->load(std::memory_order_relaxed);
  }
  return 0;
}

HistogramSnapshot TelemetryRegistry::GetUpstreamLatencySnapshot(std::string_view provider) const {
  std::string prov(provider);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = latency_histograms_.find(prov);
  if (it != latency_histograms_.end()) {
    return it->second->Snapshot();
  }
  return Histogram().Snapshot();
}

std::string TelemetryRegistry::ToPrometheusFormat() const {
  std::ostringstream oss;

  // 1. gateway_requests_total
  oss << "# HELP gateway_requests_total Total number of chat completion requests processed by the gateway\n";
  oss << "# TYPE gateway_requests_total counter\n";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, counter] : requests_counters_) {
      // Split key into model, provider, status
      auto pos1 = key.find('|');
      auto pos2 = key.rfind('|');
      if (pos1 != std::string::npos && pos2 != std::string::npos && pos1 != pos2) {
        std::string model = key.substr(0, pos1);
        std::string provider = key.substr(pos1 + 1, pos2 - pos1 - 1);
        std::string status = key.substr(pos2 + 1);
        oss << "gateway_requests_total{model=\"" << model
            << "\",provider=\"" << provider
            << "\",status=\"" << status
            << "\"} " << counter->load(std::memory_order_relaxed) << "\n";
      }
    }
  }

  // 2. gateway_cache_hits_total
  oss << "\n# HELP gateway_cache_hits_total Total number of completion requests resolved from cache tiers\n";
  oss << "# TYPE gateway_cache_hits_total counter\n";
  oss << "gateway_cache_hits_total{tier=\"exact\"} " << exact_cache_hits_.load(std::memory_order_relaxed) << "\n";
  oss << "gateway_cache_hits_total{tier=\"semantic\"} " << semantic_cache_hits_.load(std::memory_order_relaxed) << "\n";
  oss << "gateway_cache_hits_total{tier=\"miss\"} " << cache_misses_.load(std::memory_order_relaxed) << "\n";

  // 3. circuit_breaker_state
  oss << "\n# HELP circuit_breaker_state Active state of upstream provider circuit breaker (0=CLOSED, 1=OPEN, 2=HALF-OPEN)\n";
  oss << "# TYPE circuit_breaker_state gauge\n";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [provider, gauge] : circuit_gauges_) {
      oss << "circuit_breaker_state{provider=\"" << provider << "\"} "
          << gauge->load(std::memory_order_relaxed) << "\n";
    }
  }

  // 4. upstream_latency_ms
  oss << "\n# HELP upstream_latency_ms Latency of upstream LLM completions in milliseconds\n";
  oss << "# TYPE upstream_latency_ms histogram\n";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [provider, hist] : latency_histograms_) {
      HistogramSnapshot snap = hist->Snapshot();
      for (size_t i = 0; i < snap.bucket_bounds.size(); ++i) {
        oss << "upstream_latency_ms_bucket{provider=\"" << provider
            << "\",le=\"" << snap.bucket_bounds[i] << "\"} "
            << snap.bucket_counts[i] << "\n";
      }
      oss << "upstream_latency_ms_bucket{provider=\"" << provider << "\",le=\"+Inf\"} "
          << snap.total_count << "\n";
      oss << "upstream_latency_ms_sum{provider=\"" << provider << "\"} "
          << snap.sum << "\n";
      oss << "upstream_latency_ms_count{provider=\"" << provider << "\"} "
          << snap.total_count << "\n";
    }
  }

  return oss.str();
}

void TelemetryRegistry::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  requests_counters_.clear();
  exact_cache_hits_.store(0, std::memory_order_relaxed);
  semantic_cache_hits_.store(0, std::memory_order_relaxed);
  cache_misses_.store(0, std::memory_order_relaxed);
  for (auto& [_, gauge] : circuit_gauges_) {
    gauge->store(0, std::memory_order_relaxed);
  }
  for (auto& [_, hist] : latency_histograms_) {
    hist->Reset();
  }
}

}  // namespace tessera

