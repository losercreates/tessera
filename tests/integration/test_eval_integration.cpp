#include "tessera/eval/evaluation_runner.hpp"
#include "tessera/eval/evaluation_dataset.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/routing/failover_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <gtest/gtest.h>
#include <chrono>

namespace tessera {

class EvaluationIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    // 1. Setup mock upstream provider with 25ms simulated network latency
    primary_client_ = std::make_shared<MockUpstreamClient>(
        "groq", "llama-3.3-70b-versatile", "https://api.groq.com/openai/v1/chat/completions");
    primary_client_->SetDefaultResponseText("Synthesized upstream LLM answer for evaluation query.");
    primary_client_->SetSimulatedLatency(std::chrono::milliseconds(25));

    fallback_client_ = std::make_shared<MockUpstreamClient>(
        "openrouter", "google/gemma-2-9b-it", "https://openrouter.ai/api/v1/chat/completions");
    fallback_client_->SetDefaultResponseText("Synthesized fallback LLM answer.");

    CircuitBreakerConfig cb_cfg;
    cb_cfg.consecutive_failure_threshold = 5;
    primary_breaker_ = std::make_shared<CircuitBreaker>(cb_cfg);
    fallback_breaker_ = std::make_shared<CircuitBreaker>(cb_cfg);

    router_ = std::make_shared<FailoverRouter>(
        primary_client_, primary_breaker_, fallback_client_, fallback_breaker_, "google/gemma-2-9b-it");

    // 2. Setup Redis and Two-Tier Cache with exact + semantic matching
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    redis_pool_ = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis_);
    exact_cache_ = std::make_shared<ExactCacheService>(redis_pool_, 86400);

    embedding_engine_ = std::make_shared<MockEmbeddingEngine>();
    vector_store_ = std::make_shared<MockVectorIndex>();
    semantic_cache_ = std::make_shared<SemanticCacheService>(embedding_engine_, vector_store_, 0.95f, 86400);

    two_tier_cache_ = std::make_shared<TwoTierCacheService>(exact_cache_, semantic_cache_);
    cached_backend_ = std::make_shared<CachedCompletionBackend>(router_, two_tier_cache_);
  }

  void TearDown() override {
    TelemetryRegistry::Instance().Reset();
  }

  std::shared_ptr<MockUpstreamClient> primary_client_;
  std::shared_ptr<MockUpstreamClient> fallback_client_;
  std::shared_ptr<CircuitBreaker> primary_breaker_;
  std::shared_ptr<CircuitBreaker> fallback_breaker_;
  std::shared_ptr<FailoverRouter> router_;

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<ExactCacheService> exact_cache_;
  std::shared_ptr<MockEmbeddingEngine> embedding_engine_;
  std::shared_ptr<MockVectorIndex> vector_store_;
  std::shared_ptr<SemanticCacheService> semantic_cache_;
  std::shared_ptr<TwoTierCacheService> two_tier_cache_;
  std::shared_ptr<CachedCompletionBackend> cached_backend_;
};

TEST_F(EvaluationIntegrationTest, FullWorkloadExecution1000Queries) {
  EvaluationRunner runner(cached_backend_);

  EvaluationConfig cfg;
  cfg.query_count = 1000;
  cfg.paraphrase_ratio = 0.30;
  cfg.concurrency = 8;

  EvaluationReport report = runner.Run(cfg);

  // 1. Success rate must be 100%
  EXPECT_EQ(report.total_queries, 1000u);
  EXPECT_EQ(report.successful_queries, 1000u);
  EXPECT_EQ(report.failed_queries, 0u);

  // 2. Paraphrases hit Tier 2 Semantic Cache
  // Out of 300 paraphrases, at least 250 must hit semantic cache
  EXPECT_GE(report.tier2_semantic_hits, 250u);
  EXPECT_GT(report.cache_hit_ratio_pct, 25.0);

  // 3. Latency reduction verification: Warm p95 < Cold p95 with > 80% reduction
  EXPECT_GT(report.cold_p95_latency_ms, 20.0);
  EXPECT_LT(report.warm_p95_latency_ms, 10.0);
  EXPECT_GE(report.p95_latency_reduction_pct, 80.0);

  // 4. Token cost savings > 25%
  EXPECT_GE(report.token_savings_pct, 25.0);

  // 5. Memory RSS bounded strictly below 500MB
  EXPECT_LT(report.max_rss_mb, 500.0);

  // 6. Telemetry metrics verification
  EXPECT_EQ(TelemetryRegistry::Instance().GetCacheHitsTotal("semantic"), report.tier2_semantic_hits);
  EXPECT_EQ(TelemetryRegistry::Instance().GetCacheHitsTotal("miss"), report.cache_misses);
  EXPECT_GE(TelemetryRegistry::Instance().GetRequestsTotal("llama-3.3-70b-versatile", "groq", "200"), report.cache_misses);

  std::string prometheus_text = TelemetryRegistry::Instance().ToPrometheusFormat();
  EXPECT_NE(prometheus_text.find("gateway_cache_hits_total{tier=\"semantic\"}"), std::string::npos);
  EXPECT_NE(prometheus_text.find("circuit_breaker_state{provider=\"groq\"} 0"), std::string::npos);
}

TEST_F(EvaluationIntegrationTest, ReportFormattingMarkdownAndJson) {
  EvaluationRunner runner(cached_backend_);

  EvaluationConfig cfg;
  cfg.query_count = 50;
  cfg.paraphrase_ratio = 0.30;
  cfg.concurrency = 1;

  EvaluationReport report = runner.Run(cfg);

  std::string md = EvaluationRunner::FormatMarkdownReport(report);
  EXPECT_NE(md.find("# Tessera Gateway Synthetic Evaluation Report"), std::string::npos);
  EXPECT_NE(md.find("Total Queries"), std::string::npos);
  EXPECT_NE(md.find("p95 Latency Reduction"), std::string::npos);

  std::string json = EvaluationRunner::FormatJsonReport(report);
  EXPECT_NE(json.find("\"total_queries\": 50"), std::string::npos);
  EXPECT_NE(json.find("\"tier2_semantic_hits\":"), std::string::npos);
  EXPECT_NE(json.find("\"max_rss_mb\":"), std::string::npos);
}

}  // namespace tessera

