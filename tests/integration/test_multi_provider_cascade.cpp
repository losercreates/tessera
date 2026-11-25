#include <gtest/gtest.h>
#include "tessera/routing/cascade_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace tessera;

class MultiProviderCascadeIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    CircuitBreakerConfig b_cfg;
    b_cfg.consecutive_failure_threshold = 3;
    b_cfg.base_cooldown_ms = 50;
    b_cfg.max_cooldown_ms = 200;
    b_cfg.trip_on_429 = true;

    p1_client_ = std::make_shared<MockUpstreamClient>("ollama_local", "llama3.2", "http://localhost:11434");
    p1_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    p2_client_ = std::make_shared<MockUpstreamClient>("groq_cloud", "llama-3.1-8b-instant", "https://api.groq.com");
    p2_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    p3_client_ = std::make_shared<MockUpstreamClient>("openrouter_fallback", "openrouter/free", "https://openrouter.ai");
    p3_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    targets_ = {
        CascadeTarget{.client = p1_client_, .breaker = p1_breaker_, .target_model = "llama3.2", .priority = 0},
        CascadeTarget{.client = p2_client_, .breaker = p2_breaker_, .target_model = "llama-3.1-8b-instant", .priority = 1},
        CascadeTarget{.client = p3_client_, .breaker = p3_breaker_, .target_model = "openrouter/free", .priority = 2},
    };

    cascade_router_ = std::make_shared<CascadeRouter>(targets_, "resilient_3_tier");

    // Two-tier cache setup
    auto mock_redis = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    pool_cfg.min_connections = 2;
    pool_cfg.max_connections = 4;
    auto pool = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis);

    auto exact_cache = std::make_shared<ExactCacheService>(pool, 3600);
    auto emb_engine = std::make_shared<MockEmbeddingEngine>();
    auto vec_store = std::make_shared<MockVectorIndex>();
    auto sem_cache = std::make_shared<SemanticCacheService>(emb_engine, vec_store, 0.95f, 3600);
    two_tier_cache_ = std::make_shared<TwoTierCacheService>(exact_cache, sem_cache);

    cached_backend_ = std::make_shared<CachedCompletionBackend>(cascade_router_, two_tier_cache_);
  }

  std::shared_ptr<MockUpstreamClient> p1_client_;
  std::shared_ptr<CircuitBreaker> p1_breaker_;
  std::shared_ptr<MockUpstreamClient> p2_client_;
  std::shared_ptr<CircuitBreaker> p2_breaker_;
  std::shared_ptr<MockUpstreamClient> p3_client_;
  std::shared_ptr<CircuitBreaker> p3_breaker_;
  std::vector<CascadeTarget> targets_;
  std::shared_ptr<CascadeRouter> cascade_router_;
  std::shared_ptr<TwoTierCacheService> two_tier_cache_;
  std::shared_ptr<CachedCompletionBackend> cached_backend_;
};

TEST_F(MultiProviderCascadeIntegrationTest, ChaosOutageTransparentFailoverZeroLoss) {
  // P1 local fails with 429 (immediate trip)
  p1_client_->SetDefaultStatusCode(429);
  // P2 cloud fails with 500
  p2_client_->SetDefaultStatusCode(500);
  // P3 fallback succeeds
  p3_client_->SetDefaultResponseText("Successful response from fallback OpenRouter tier");

  constexpr size_t kWorkers = 8;
  constexpr size_t kReqsPerWorker = 10;
  std::vector<std::thread> threads;
  std::atomic<size_t> success_count{0};
  std::atomic<size_t> failure_count{0};

  for (size_t t = 0; t < kWorkers; ++t) {
    threads.emplace_back([this, t, &success_count, &failure_count]() {
      for (size_t r = 0; r < kReqsPerWorker; ++r) {
        ChatCompletionRequest req;
        req.correlation_id = "chaos-" + std::to_string(t) + "-" + std::to_string(r);
        req.messages.push_back(ChatMessage{.role = "user", .content = "prompt-" + std::to_string(r)});
        req.no_cache = true;

        ChatCompletionResponse resp;
        Status st = cascade_router_->Complete(req, &resp);
        if (st.ok() && resp.choices.size() > 0 &&
            resp.choices[0].message.content == "Successful response from fallback OpenRouter tier") {
          success_count.fetch_add(1);
        } else {
          failure_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // 100% completion rate - Zero dropped calls!
  EXPECT_EQ(success_count.load(), kWorkers * kReqsPerWorker);
  EXPECT_EQ(failure_count.load(), 0u);

  // Both P1 and P2 tripped to OPEN
  EXPECT_EQ(p1_breaker_->GetState(), CircuitState::OPEN);
  EXPECT_EQ(p2_breaker_->GetState(), CircuitState::OPEN);

  // Stats validation
  auto stats = cascade_router_->GetStats();
  EXPECT_EQ(stats.total_requests, kWorkers * kReqsPerWorker);
  EXPECT_EQ(stats.successful_requests, kWorkers * kReqsPerWorker);
  EXPECT_GT(stats.direct_bypasses, 0u);
  EXPECT_EQ(stats.all_providers_failed, 0u);
}

TEST_F(MultiProviderCascadeIntegrationTest, CachePreemptionBypassesCascadeDispatch) {
  p1_client_->SetDefaultResponseText("Fresh response from tier 1");

  ChatCompletionRequest req;
  req.correlation_id = "cache-preempt-test";
  req.messages.push_back(ChatMessage{.role = "user", .content = "What is the speed of light?"});

  // Query 1: Cache miss -> reaches CascadeRouter -> P1 client is called once
  ChatCompletionResponse resp1;
  Status st1 = cached_backend_->Complete(req, &resp1);
  EXPECT_TRUE(st1.ok());
  EXPECT_EQ(resp1.choices[0].message.content, "Fresh response from tier 1");
  EXPECT_EQ(p1_client_->CallCount(), 1u);
  EXPECT_FALSE(resp1.cache_hit);

  // Query 2: Identical prompt -> Cache HIT!
  ChatCompletionResponse resp2;
  Status st2 = cached_backend_->Complete(req, &resp2);
  EXPECT_TRUE(st2.ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(resp2.choices[0].message.content, "Fresh response from tier 1");

  // CRITICAL VERIFICATION: P1 was NOT dispatched a second time!
  EXPECT_EQ(p1_client_->CallCount(), 1u);
  EXPECT_EQ(p2_client_->CallCount(), 0u);
  EXPECT_EQ(p3_client_->CallCount(), 0u);

  // Even if P1 is now broken with 429, the cache serves the query without tripping breakers!
  p1_client_->SetDefaultStatusCode(429);
  ChatCompletionResponse resp3;
  Status st3 = cached_backend_->Complete(req, &resp3);
  EXPECT_TRUE(st3.ok());
  EXPECT_TRUE(resp3.cache_hit);
  EXPECT_EQ(p1_breaker_->GetState(), CircuitState::CLOSED); // Breaker untouched!
  EXPECT_EQ(p1_client_->CallCount(), 1u);
}

TEST_F(MultiProviderCascadeIntegrationTest, FallbackCompletionPopulatesCacheAndPreemptsQuota) {
  auto rl_mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pool_cfg;
  pool_cfg.min_connections = 2;
  pool_cfg.max_connections = 4;
  auto rl_pool = std::make_shared<RedisConnectionPool>(pool_cfg, rl_mock_redis);
  auto limiter = std::make_shared<DistributedRateLimiter>(rl_pool);

  // P1 quota exhausted immediately via 1 initial call; P2 has 5 capacity
  limiter->AddRule(RateLimitRule{
      .provider = "ollama_local",
      .window_name = "minute",
      .capacity = 1.0,
      .refill_rate_per_sec = 1.0 / 60.0,
      .ttl_seconds = 60,
  });
  limiter->AddRule(RateLimitRule{
      .provider = "groq_cloud",
      .window_name = "minute",
      .capacity = 5.0,
      .refill_rate_per_sec = 5.0 / 60.0,
      .ttl_seconds = 60,
  });

  // Reset P1 and P2 fresh clients wrapped in a quota-aware CascadeRouter + CachedCompletionBackend
  p1_client_->SetDefaultResponseText("P1 primary reply");
  p2_client_->SetDefaultResponseText("P2 fallback recovered reply");
  auto quota_cascade = std::make_shared<CascadeRouter>(targets_, "quota-cached-cascade", limiter);
  auto stack = std::make_shared<CachedCompletionBackend>(quota_cascade, two_tier_cache_);

  // Exhaust P1's sole token directly via an uncached prompt
  ChatCompletionRequest req_drain;
  req_drain.correlation_id = "drain-p1";
  req_drain.messages.push_back(ChatMessage{
      .role = "user",
      .content = "What is the capital of france paris europe germany berlin"});
  ChatCompletionResponse resp_drain;
  ASSERT_TRUE(stack->Complete(req_drain, &resp_drain).ok());
  EXPECT_EQ(p1_client_->CallCount(), 1u);

  // Next uncached request misses P1 due to exhausted local quota -> fails over to P2 and populates caches!
  ChatCompletionRequest req_failover;
  req_failover.correlation_id = "fallback-warm";
  req_failover.messages.push_back(ChatMessage{
      .role = "user",
      .content = "Explain quantum physics science computer programming gateway cache semantic exact redis"});
  ChatCompletionResponse resp_failover;
  ASSERT_TRUE(stack->Complete(req_failover, &resp_failover).ok());
  EXPECT_FALSE(resp_failover.cache_hit);
  EXPECT_EQ(resp_failover.choices[0].message.content, "P2 fallback recovered reply");
  EXPECT_EQ(p1_client_->CallCount(), 1u); // P1 skipped!
  EXPECT_EQ(p2_client_->CallCount(), 1u); // P2 served & populated cache

  uint64_t lua_evals_before_replay = rl_mock_redis->GetSharedState()->lua_eval_count.load();

  // Exact replay of fallback-populated entry -> Tier 1 hit, 0 Lua calls, 0 upstream calls
  ChatCompletionResponse resp_exact;
  ASSERT_TRUE(stack->Complete(req_failover, &resp_exact).ok());
  EXPECT_TRUE(resp_exact.cache_hit);
  EXPECT_EQ(resp_exact.cache_tier, "exact");
  EXPECT_EQ(resp_exact.choices[0].message.content, "P2 fallback recovered reply");
  EXPECT_EQ(rl_mock_redis->GetSharedState()->lua_eval_count.load(), lua_evals_before_replay);
  EXPECT_EQ(p1_client_->CallCount(), 1u);
  EXPECT_EQ(p2_client_->CallCount(), 1u);
}
