#include <gtest/gtest.h>
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/routing/model_router.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/routing/failover_router.hpp"
#include "tessera/routing/provider_registry.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"

using namespace tessera;

class CacheFirstCascadeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    pool_cfg.min_connections = 2;
    pool_cfg.max_connections = 8;
    redis_pool_ = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis_);

    exact_cache_ = std::make_shared<ExactCacheService>(redis_pool_, 3600);
    embedding_engine_ = std::make_shared<MockEmbeddingEngine>();
    vector_store_ = std::make_shared<MockVectorIndex>();
    semantic_cache_ = std::make_shared<SemanticCacheService>(
        embedding_engine_, vector_store_, 0.95f, 3600);
    two_tier_cache_ = std::make_shared<TwoTierCacheService>(exact_cache_, semantic_cache_);

    rate_limiter_ = std::make_shared<DistributedRateLimiter>(redis_pool_);
    registry_.SetRateLimiter(rate_limiter_);
    registry_.SetClientFactory([](const ProviderConfig& cfg) {
      return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
    });

    // Primary provider with tiny quota of 2 requests/min
    ProviderConfig p1;
    p1.name = "primary_prov";
    p1.endpoint = "https://primary.api/v1/chat/completions";
    p1.default_model = "primary-fast";
    p1.rate_limits.push_back(RateLimitRule{
        .provider = "primary_prov",
        .window_name = "minute",
        .capacity = 2.0,
        .refill_rate_per_sec = 2.0 / 60.0,
        .ttl_seconds = 60,
    });
    ASSERT_TRUE(registry_.RegisterProvider(p1));

    p1_client_ = std::dynamic_pointer_cast<MockUpstreamClient>(
        registry_.GetProviderClient("primary_prov"));
    ASSERT_NE(p1_client_, nullptr);
    p1_client_->SetDefaultResponseText("Answer from primary_prov");

    model_router_ = ModelRouter::CreateFromRegistry(registry_, rate_limiter_);
    pipeline_ = std::make_shared<CachedCompletionBackend>(model_router_, two_tier_cache_);
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<ExactCacheService> exact_cache_;
  std::shared_ptr<MockEmbeddingEngine> embedding_engine_;
  std::shared_ptr<MockVectorIndex> vector_store_;
  std::shared_ptr<SemanticCacheService> semantic_cache_;
  std::shared_ptr<TwoTierCacheService> two_tier_cache_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
  ProviderRegistry registry_;
  std::shared_ptr<MockUpstreamClient> p1_client_;
  std::shared_ptr<ModelRouter> model_router_;
  std::shared_ptr<CachedCompletionBackend> pipeline_;
};

TEST_F(CacheFirstCascadeTest, ExactAndSemanticHitsConsumeZeroQuotaEvenWhenSaturated) {
  auto shared_state = mock_redis_->GetSharedState();

  // 1. Prime cache with P1 (cache miss -> uses 1 token; 1 remaining)
  ChatCompletionRequest req_p1;
  req_p1.model = "primary-fast";
  req_p1.correlation_id = "c-p1";
  req_p1.messages.push_back(ChatMessage{
      .role = "user",
      .content = "Explain how distributed consensus algorithms maintain safety across network partitions in large clusters today"});

  ChatCompletionResponse resp1;
  ASSERT_TRUE(pipeline_->Complete(req_p1, &resp1).ok());
  EXPECT_FALSE(resp1.cache_hit);
  EXPECT_EQ(p1_client_->CallCount(), 1u);
  EXPECT_EQ(shared_state->lua_eval_count.load(), 1u);

  // 2. Exhaust the second (and final) token with a completely different uncached prompt P2
  ChatCompletionRequest req_p2;
  req_p2.model = "primary-fast";
  req_p2.correlation_id = "c-p2";
  req_p2.messages.push_back(ChatMessage{.role = "user", .content = "Summarize quantum entanglement briefly."});

  ChatCompletionResponse resp2;
  ASSERT_TRUE(pipeline_->Complete(req_p2, &resp2).ok());
  EXPECT_FALSE(resp2.cache_hit);
  EXPECT_EQ(p1_client_->CallCount(), 2u);
  EXPECT_EQ(shared_state->lua_eval_count.load(), 2u);

  // Confirm primary_prov bucket is now at 0 tokens (uncached P3 is rejected)
  ChatCompletionRequest req_p3;
  req_p3.model = "primary-fast";
  req_p3.correlation_id = "c-p3";
  req_p3.messages.push_back(ChatMessage{.role = "user", .content = "Uncached query number three"});
  ChatCompletionResponse resp3;
  Status st3 = pipeline_->Complete(req_p3, &resp3);
  EXPECT_FALSE(st3.ok());
  EXPECT_EQ(st3.code, StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_EQ(p1_client_->CallCount(), 2u);
  uint64_t evals_after_exhaustion = shared_state->lua_eval_count.load();

  // 3. Replay P1 multiple times under 0 remaining tokens -> Tier 1 EXACT HIT, 0 quota/Lua calls!
  for (int i = 0; i < 5; ++i) {
    ChatCompletionResponse hit_resp;
    req_p1.correlation_id = "c-p1-exact-" + std::to_string(i);
    Status hit_st = pipeline_->Complete(req_p1, &hit_resp);
    ASSERT_TRUE(hit_st.ok());
    EXPECT_TRUE(hit_resp.cache_hit);
    EXPECT_EQ(hit_resp.cache_tier, "exact");
  }
  EXPECT_EQ(p1_client_->CallCount(), 2u);
  EXPECT_EQ(shared_state->lua_eval_count.load(), evals_after_exhaustion);

  // 4. Send close paraphrase of P1 -> Tier 2 SEMANTIC HIT under 0 remaining tokens, 0 Lua calls!
  ChatCompletionRequest req_p1_para;
  req_p1_para.model = "primary-fast";
  req_p1_para.correlation_id = "c-p1-sem";
  req_p1_para.messages.push_back(ChatMessage{
      .role = "user",
      .content = "Explain how distributed consensus algorithms maintain safety across network partitions in large clusters now"});

  // Confirm similarity exceeds threshold deterministically before lookup
  PromptEmbedding emb_orig = embedding_engine_->GenerateEmbedding(req_p1.messages[0].content);
  PromptEmbedding emb_para = embedding_engine_->GenerateEmbedding(req_p1_para.messages[0].content);
  ASSERT_GE(PromptEmbedding::CosineSimilarity(emb_orig, emb_para), 0.95f);

  ChatCompletionResponse sem_resp;
  Status sem_st = pipeline_->Complete(req_p1_para, &sem_resp);
  ASSERT_TRUE(sem_st.ok());
  EXPECT_TRUE(sem_resp.cache_hit);
  EXPECT_EQ(sem_resp.cache_tier, "semantic");
  EXPECT_EQ(p1_client_->CallCount(), 2u);
  EXPECT_EQ(shared_state->lua_eval_count.load(), evals_after_exhaustion);
}

TEST_F(CacheFirstCascadeTest, MixedNoCacheBypassHitsQuotaWhileCachedReplaysSucceed) {
  auto shared_state = mock_redis_->GetSharedState();

  // Warm cache with 1 token (1 left)
  ChatCompletionRequest req;
  req.model = "primary-fast";
  req.correlation_id = "c-warm";
  req.messages.push_back(ChatMessage{.role = "user", .content = "What is consistent hashing?"});

  ChatCompletionResponse warm_resp;
  ASSERT_TRUE(pipeline_->Complete(req, &warm_resp).ok());
  EXPECT_EQ(shared_state->lua_eval_count.load(), 1u);

  // Explicit cache bypass (no_cache = true) consumes the final token
  ChatCompletionRequest bypass_req = req;
  bypass_req.no_cache = true;
  bypass_req.correlation_id = "c-bypass-1";
  ChatCompletionResponse bypass_resp1;
  ASSERT_TRUE(pipeline_->Complete(bypass_req, &bypass_resp1).ok());
  EXPECT_FALSE(bypass_resp1.cache_hit);
  EXPECT_EQ(p1_client_->CallCount(), 2u);
  EXPECT_EQ(shared_state->lua_eval_count.load(), 2u);

  // Second no_cache bypass hits empty bucket -> rejected with RESOURCE_EXHAUSTED
  bypass_req.correlation_id = "c-bypass-2";
  ChatCompletionResponse bypass_resp2;
  Status st_bypass = pipeline_->Complete(bypass_req, &bypass_resp2);
  EXPECT_FALSE(st_bypass.ok());
  EXPECT_EQ(st_bypass.code, StatusCode::RESOURCE_EXHAUSTED);
  uint64_t evals_after_bypass_fail = shared_state->lua_eval_count.load();

  // Concurrent/subsequent normal replay (no_cache = false) STILL hits exact cache with 0 Lua calls!
  req.correlation_id = "c-normal-replay";
  ChatCompletionResponse normal_resp;
  ASSERT_TRUE(pipeline_->Complete(req, &normal_resp).ok());
  EXPECT_TRUE(normal_resp.cache_hit);
  EXPECT_EQ(normal_resp.cache_tier, "exact");
  EXPECT_EQ(shared_state->lua_eval_count.load(), evals_after_bypass_fail);
}

TEST_F(CacheFirstCascadeTest, FailoverRouterInheritsQuotaAwareFailoverAndResourceExhausted) {
  auto fr_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pool_cfg;
  pool_cfg.min_connections = 1;
  pool_cfg.max_connections = 4;
  auto fr_pool = std::make_shared<RedisConnectionPool>(pool_cfg, fr_redis);
  auto fr_limiter = std::make_shared<DistributedRateLimiter>(fr_pool);

  fr_limiter->AddRule(RateLimitRule{
      .provider = "fr_primary",
      .window_name = "minute",
      .capacity = 1.0,
      .refill_rate_per_sec = 1.0 / 60.0,
      .ttl_seconds = 60,
  });
  fr_limiter->AddRule(RateLimitRule{
      .provider = "fr_fallback",
      .window_name = "minute",
      .capacity = 1.0,
      .refill_rate_per_sec = 1.0 / 60.0,
      .ttl_seconds = 60,
  });

  auto primary_client = std::make_shared<MockUpstreamClient>(
      "fr_primary", "primary-m", "https://fr-primary.local/v1/chat/completions");
  primary_client->SetDefaultResponseText("from_fr_primary");
  auto fallback_client = std::make_shared<MockUpstreamClient>(
      "fr_fallback", "fallback-m", "https://fr-fallback.local/v1/chat/completions");
  fallback_client->SetDefaultResponseText("from_fr_fallback");

  CircuitBreakerConfig cb_cfg;
  cb_cfg.consecutive_failure_threshold = 3;
  cb_cfg.base_cooldown_ms = 100;
  cb_cfg.max_cooldown_ms = 1000;
  cb_cfg.trip_on_429 = true;
  auto primary_breaker = std::make_shared<CircuitBreaker>(cb_cfg);
  auto fallback_breaker = std::make_shared<CircuitBreaker>(cb_cfg);

  FailoverRouter router(primary_client, primary_breaker, fallback_client,
                        fallback_breaker, "fallback-m", fr_limiter);

  auto make_req = [](std::string cid) {
    ChatCompletionRequest req;
    req.model = "primary-m";
    req.correlation_id = std::move(cid);
    req.messages.push_back(ChatMessage{.role = "user", .content = "Hello"});
    return req;
  };

  // Request 1 succeeds on primary (consumes primary's 1 token)
  ChatCompletionResponse resp1;
  ASSERT_TRUE(router.Complete(make_req("fr-1"), &resp1).ok());
  EXPECT_EQ(primary_client->CallCount(), 1u);
  EXPECT_EQ(fallback_client->CallCount(), 0u);

  // Request 2 skips primary due to local quota depletion and succeeds on fallback
  ChatCompletionResponse resp2;
  ASSERT_TRUE(router.Complete(make_req("fr-2"), &resp2).ok());
  EXPECT_EQ(primary_client->CallCount(), 1u);
  EXPECT_EQ(fallback_client->CallCount(), 1u);
  EXPECT_EQ(primary_breaker->GetState(), CircuitState::CLOSED);

  // Request 3 finds both tiers locally exhausted -> returns RESOURCE_EXHAUSTED
  ChatCompletionResponse resp3;
  Status st3 = router.Complete(make_req("fr-3"), &resp3);
  EXPECT_FALSE(st3.ok());
  EXPECT_EQ(st3.code, StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_EQ(primary_client->CallCount(), 1u);
  EXPECT_EQ(fallback_client->CallCount(), 1u);
  EXPECT_EQ(primary_breaker->GetState(), CircuitState::CLOSED);
  EXPECT_EQ(fallback_breaker->GetState(), CircuitState::CLOSED);

  FailoverRouterStats stats = router.GetStats();
  EXPECT_EQ(stats.rate_limit_skips, 3u);
  EXPECT_EQ(stats.failover_attempts, 1u);
}
