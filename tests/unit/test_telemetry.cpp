#include "tessera/telemetry/telemetry_registry.hpp"
#include <gtest/gtest.h>

namespace tessera {

class TelemetryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();
  }

  void TearDown() override {
    TelemetryRegistry::Instance().Reset();
  }
};

TEST_F(TelemetryTest, RequestsTotalIncrementsPerLabel) {
  auto& registry = TelemetryRegistry::Instance();

  registry.IncrementRequestsTotal("llama-3.3-70b-versatile", "groq", "200");
  registry.IncrementRequestsTotal("llama-3.3-70b-versatile", "groq", "200");
  registry.IncrementRequestsTotal("llama-3.3-70b-versatile", "groq", "429");
  registry.IncrementRequestsTotal("google/gemma-2-9b-it", "openrouter", "200");

  EXPECT_EQ(registry.GetRequestsTotal("llama-3.3-70b-versatile", "groq", "200"), 2u);
  EXPECT_EQ(registry.GetRequestsTotal("llama-3.3-70b-versatile", "groq", "429"), 1u);
  EXPECT_EQ(registry.GetRequestsTotal("google/gemma-2-9b-it", "openrouter", "200"), 1u);
  EXPECT_EQ(registry.GetRequestsTotal("non-existent", "groq", "200"), 0u);
}

TEST_F(TelemetryTest, CacheHitsTierPartitioning) {
  auto& registry = TelemetryRegistry::Instance();

  registry.IncrementCacheHitsTotal("exact");
  registry.IncrementCacheHitsTotal("exact");
  registry.IncrementCacheHitsTotal("semantic");
  registry.IncrementCacheHitsTotal("miss");
  registry.IncrementCacheHitsTotal("miss");
  registry.IncrementCacheHitsTotal("miss");

  EXPECT_EQ(registry.GetCacheHitsTotal("exact"), 2u);
  EXPECT_EQ(registry.GetCacheHitsTotal("semantic"), 1u);
  EXPECT_EQ(registry.GetCacheHitsTotal("miss"), 3u);
}

TEST_F(TelemetryTest, CircuitBreakerStateGauges) {
  auto& registry = TelemetryRegistry::Instance();

  EXPECT_EQ(registry.GetCircuitBreakerState("groq"), 0u); // CLOSED

  registry.SetCircuitBreakerState("groq", CircuitState::OPEN);
  EXPECT_EQ(registry.GetCircuitBreakerState("groq"), 1u);

  registry.SetCircuitBreakerState("groq", CircuitState::HALF_OPEN);
  EXPECT_EQ(registry.GetCircuitBreakerState("groq"), 2u);

  registry.SetCircuitBreakerState("groq", CircuitState::CLOSED);
  EXPECT_EQ(registry.GetCircuitBreakerState("groq"), 0u);
}

TEST_F(TelemetryTest, HistogramBucketDistribution) {
  auto& registry = TelemetryRegistry::Instance();

  registry.RecordUpstreamLatency("groq", 0.5);   // <= 1.0
  registry.RecordUpstreamLatency("groq", 4.2);   // <= 5.0
  registry.RecordUpstreamLatency("groq", 45.0);  // <= 50.0
  registry.RecordUpstreamLatency("groq", 200.0); // <= 250.0

  auto snap = registry.GetUpstreamLatencySnapshot("groq");
  EXPECT_EQ(snap.total_count, 4u);
  EXPECT_NEAR(snap.sum, 249.7, 0.5);

  // Check cumulative bucket counts
  // Bucket 1.0 should have 1 count (0.5)
  EXPECT_GE(snap.bucket_counts[0], 1u);
}

TEST_F(TelemetryTest, PrometheusTextFormatSerialization) {
  auto& registry = TelemetryRegistry::Instance();

  registry.IncrementRequestsTotal("llama-3.3-70b-versatile", "groq", "200");
  registry.IncrementCacheHitsTotal("exact");
  registry.IncrementCacheHitsTotal("semantic");
  registry.SetCircuitBreakerState("groq", CircuitState::CLOSED);
  registry.RecordUpstreamLatency("groq", 12.5);

  std::string output = registry.ToPrometheusFormat();

  EXPECT_NE(output.find("# HELP gateway_requests_total"), std::string::npos);
  EXPECT_NE(output.find("# TYPE gateway_requests_total counter"), std::string::npos);
  EXPECT_NE(output.find("gateway_requests_total{model=\"llama-3.3-70b-versatile\",provider=\"groq\",status=\"200\"} 1"), std::string::npos);

  EXPECT_NE(output.find("# HELP gateway_cache_hits_total"), std::string::npos);
  EXPECT_NE(output.find("# TYPE gateway_cache_hits_total counter"), std::string::npos);
  EXPECT_NE(output.find("gateway_cache_hits_total{tier=\"exact\"} 1"), std::string::npos);
  EXPECT_NE(output.find("gateway_cache_hits_total{tier=\"semantic\"} 1"), std::string::npos);

  EXPECT_NE(output.find("# HELP circuit_breaker_state"), std::string::npos);
  EXPECT_NE(output.find("circuit_breaker_state{provider=\"groq\"} 0"), std::string::npos);

  EXPECT_NE(output.find("# HELP upstream_latency_ms"), std::string::npos);
  EXPECT_NE(output.find("# TYPE upstream_latency_ms histogram"), std::string::npos);
}

TEST_F(TelemetryTest, DynamicMultiProviderRegistrationAndExposition) {
  auto& registry = TelemetryRegistry::Instance();

  // Dynamically register 3 new providers
  registry.RegisterProvider("ollama");
  registry.RegisterProvider("deepseek");
  registry.RegisterProvider("mistral");

  auto tracked = registry.GetTrackedProviders();
  EXPECT_GE(tracked.size(), 5u); // groq, openrouter, ollama, deepseek, mistral

  // Record operational telemetry across the dynamic backends
  registry.IncrementRequestsTotal("llama3.2", "ollama", "200");
  registry.IncrementRequestsTotal("llama3.2", "ollama", "200");
  registry.IncrementRequestsTotal("deepseek-chat", "deepseek", "503");
  registry.IncrementRequestsTotal("mistral-large", "mistral", "200");

  registry.SetCircuitBreakerState("ollama", CircuitState::CLOSED);
  registry.SetCircuitBreakerState("deepseek", CircuitState::OPEN);
  registry.SetCircuitBreakerState("mistral", CircuitState::HALF_OPEN);

  registry.RecordUpstreamLatency("ollama", 18.2);
  registry.RecordUpstreamLatency("deepseek", 420.5);
  registry.RecordUpstreamLatency("mistral", 95.0);

  // Validate inspection hooks
  EXPECT_EQ(registry.GetRequestsTotal("llama3.2", "ollama", "200"), 2u);
  EXPECT_EQ(registry.GetRequestsTotal("deepseek-chat", "deepseek", "503"), 1u);
  EXPECT_EQ(registry.GetRequestsTotal("mistral-large", "mistral", "200"), 1u);

  EXPECT_EQ(registry.GetCircuitBreakerState("ollama"), 0u);
  EXPECT_EQ(registry.GetCircuitBreakerState("deepseek"), 1u);
  EXPECT_EQ(registry.GetCircuitBreakerState("mistral"), 2u);

  auto ollama_lat = registry.GetUpstreamLatencySnapshot("ollama");
  EXPECT_EQ(ollama_lat.total_count, 1u);
  EXPECT_NEAR(ollama_lat.sum, 18.2, 0.1);

  // Validate Prometheus exposition text
  std::string prom = registry.ToPrometheusFormat();

  // Counters
  EXPECT_NE(prom.find("gateway_requests_total{model=\"llama3.2\",provider=\"ollama\",status=\"200\"} 2"), std::string::npos);
  EXPECT_NE(prom.find("gateway_requests_total{model=\"deepseek-chat\",provider=\"deepseek\",status=\"503\"} 1"), std::string::npos);
  EXPECT_NE(prom.find("gateway_requests_total{model=\"mistral-large\",provider=\"mistral\",status=\"200\"} 1"), std::string::npos);

  // Gauges
  EXPECT_NE(prom.find("circuit_breaker_state{provider=\"ollama\"} 0"), std::string::npos);
  EXPECT_NE(prom.find("circuit_breaker_state{provider=\"deepseek\"} 1"), std::string::npos);
  EXPECT_NE(prom.find("circuit_breaker_state{provider=\"mistral\"} 2"), std::string::npos);

  // Histograms
  EXPECT_NE(prom.find("upstream_latency_ms_count{provider=\"ollama\"} 1"), std::string::npos);
  EXPECT_NE(prom.find("upstream_latency_ms_count{provider=\"deepseek\"} 1"), std::string::npos);
  EXPECT_NE(prom.find("upstream_latency_ms_count{provider=\"mistral\"} 1"), std::string::npos);
  EXPECT_NE(prom.find("upstream_latency_ms_sum{provider=\"ollama\"}"), std::string::npos);
}

TEST_F(TelemetryTest, RateLimitSkipsCounterIsolatedFromLatencyAndReset) {
  auto& registry = TelemetryRegistry::Instance();

  registry.RegisterProvider("primary_p");
  registry.RegisterProvider("fallback_p");

  // Pre-seeded to 0 upon registration
  EXPECT_EQ(registry.GetRateLimitSkipsTotal("primary_p"), 0u);
  EXPECT_EQ(registry.GetRateLimitSkipsTotal("fallback_p"), 0u);

  registry.IncrementRateLimitSkipsTotal("primary_p");
  registry.IncrementRateLimitSkipsTotal("primary_p");
  EXPECT_EQ(registry.GetRateLimitSkipsTotal("primary_p"), 2u);
  EXPECT_EQ(registry.GetRateLimitSkipsTotal("fallback_p"), 0u);

  // Skipped provider MUST NOT gain synthetic upstream latency samples
  EXPECT_EQ(registry.GetUpstreamLatencySnapshot("primary_p").total_count, 0u);

  std::string prom = registry.ToPrometheusFormat();
  EXPECT_NE(prom.find("# HELP gateway_ratelimit_skips_total"), std::string::npos);
  EXPECT_NE(prom.find("# TYPE gateway_ratelimit_skips_total counter"), std::string::npos);
  EXPECT_NE(prom.find("gateway_ratelimit_skips_total{provider=\"primary_p\"} 2"), std::string::npos);
  EXPECT_NE(prom.find("gateway_ratelimit_skips_total{provider=\"fallback_p\"} 0"), std::string::npos);

  // Reset must clear dynamic rate limit skip counters cleanly
  registry.Reset();
  EXPECT_EQ(registry.GetRateLimitSkipsTotal("primary_p"), 0u);
}

}  // namespace tessera

