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

}  // namespace tessera

