#include <gtest/gtest.h>
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/routing/provider_registry.hpp"
#include "tessera/routing/model_router.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <vector>

using namespace tessera;

class ProviderRateIsolationIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    pool_cfg.min_connections = 4;
    pool_cfg.max_connections = 32;
    redis_pool_ = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis_);
    rate_limiter_ = std::make_shared<DistributedRateLimiter>(redis_pool_);
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
};

TEST_F(ProviderRateIsolationIntegrationTest, SequentialQuotaIsolation) {
  // Provider A: 5 RPM
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "provider_a",
      .window_name = "minute",
      .capacity = 5.0,
      .refill_rate_per_sec = 5.0 / 60.0,
      .ttl_seconds = 60,
  });

  // Provider B: 50 RPM
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "provider_b",
      .window_name = "minute",
      .capacity = 50.0,
      .refill_rate_per_sec = 50.0 / 60.0,
      .ttl_seconds = 60,
  });

  // Exhaust Provider A
  for (int i = 0; i < 5; ++i) {
    auto res = rate_limiter_->CheckAndConsume("provider_a", 1.0);
    EXPECT_TRUE(res.allowed);
    EXPECT_EQ(res.provider, "provider_a");
  }

  // 6th request to Provider A must be rejected
  auto a_rejected = rate_limiter_->CheckAndConsume("provider_a", 1.0);
  EXPECT_FALSE(a_rejected.allowed);
  EXPECT_EQ(a_rejected.remaining_tokens, 0u);
  EXPECT_GT(a_rejected.retry_after_ms, 0u);

  // Provider B must still have full quota unhindered
  for (int i = 0; i < 20; ++i) {
    auto res = rate_limiter_->CheckAndConsume("provider_b", 1.0);
    EXPECT_TRUE(res.allowed);
    EXPECT_EQ(res.provider, "provider_b");
  }

  // Verify Provider B has remaining tokens (50 - 20 = 30)
  auto b_status = rate_limiter_->CheckAndConsume("provider_b", 1.0);
  EXPECT_TRUE(b_status.allowed);
  EXPECT_EQ(b_status.remaining_tokens, 29u);

  // Confirm Provider A is still rejected
  EXPECT_FALSE(rate_limiter_->CheckAndConsume("provider_a", 1.0).allowed);
}

TEST_F(ProviderRateIsolationIntegrationTest, ConcurrentQuotaSaturationUnderLoad) {
  // Provider Alpha: 15 RPM
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "alpha",
      .window_name = "minute",
      .capacity = 15.0,
      .refill_rate_per_sec = 15.0 / 60.0,
      .ttl_seconds = 60,
  });

  // Provider Beta: 25 RPM
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "beta",
      .window_name = "minute",
      .capacity = 25.0,
      .refill_rate_per_sec = 25.0 / 60.0,
      .ttl_seconds = 60,
  });

  constexpr size_t kAlphaThreads = 80;
  constexpr size_t kBetaThreads = 80;
  constexpr size_t kTotalThreads = kAlphaThreads + kBetaThreads;

  std::atomic<size_t> alpha_allowed{0};
  std::atomic<size_t> alpha_rejected{0};
  std::atomic<size_t> beta_allowed{0};
  std::atomic<size_t> beta_rejected{0};

  std::barrier barrier(static_cast<ptrdiff_t>(kTotalThreads));
  std::vector<std::thread> threads;
  threads.reserve(kTotalThreads);

  for (size_t i = 0; i < kAlphaThreads; ++i) {
    threads.emplace_back([&]() {
      barrier.arrive_and_wait();
      auto res = rate_limiter_->CheckAndConsume("alpha", 1.0);
      if (res.allowed) {
        alpha_allowed.fetch_add(1, std::memory_order_relaxed);
      } else {
        alpha_rejected.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (size_t i = 0; i < kBetaThreads; ++i) {
    threads.emplace_back([&]() {
      barrier.arrive_and_wait();
      auto res = rate_limiter_->CheckAndConsume("beta", 1.0);
      if (res.allowed) {
        beta_allowed.fetch_add(1, std::memory_order_relaxed);
      } else {
        beta_rejected.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(alpha_allowed.load(), 15u);
  EXPECT_EQ(alpha_rejected.load(), kAlphaThreads - 15u);

  EXPECT_EQ(beta_allowed.load(), 25u);
  EXPECT_EQ(beta_rejected.load(), kBetaThreads - 25u);
}

TEST_F(ProviderRateIsolationIntegrationTest, CircuitBreakerFaultIsolation) {
  CircuitBreakerConfig cfg;
  cfg.consecutive_failure_threshold = 3;
  cfg.base_cooldown_ms = 10;
  cfg.max_cooldown_ms = 20;
  cfg.trip_on_429 = true;

  CircuitBreaker breaker_a(cfg);
  CircuitBreaker breaker_b(cfg);

  EXPECT_EQ(breaker_a.GetState(), CircuitState::CLOSED);
  EXPECT_EQ(breaker_b.GetState(), CircuitState::CLOSED);

  // Trip breaker A with 3 consecutive 500 errors
  breaker_a.RecordFailure(500, "Server Error");
  breaker_a.RecordFailure(500, "Server Error");
  breaker_a.RecordFailure(500, "Server Error");

  EXPECT_EQ(breaker_a.GetState(), CircuitState::OPEN);
  EXPECT_FALSE(breaker_a.CanAttempt());

  // Breaker B must be completely unaffected and remain CLOSED
  EXPECT_EQ(breaker_b.GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_b.CanAttempt());
  breaker_b.RecordSuccess();
  EXPECT_EQ(breaker_b.GetState(), CircuitState::CLOSED);

  // Wait for Breaker A cooldown (at most 20ms)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Breaker A transitions to HALF-OPEN upon trial probe evaluation
  EXPECT_TRUE(breaker_a.CanAttempt());
  EXPECT_EQ(breaker_a.GetState(), CircuitState::HALF_OPEN);
  // Second consecutive call to CanAttempt() in HALF-OPEN should be false (only 1 probe permitted)
  EXPECT_FALSE(breaker_a.CanAttempt());

  // Breaker B continues allowing multiple requests freely
  EXPECT_TRUE(breaker_b.CanAttempt());
  EXPECT_TRUE(breaker_b.CanAttempt());
  EXPECT_TRUE(breaker_b.CanAttempt());
}

TEST_F(ProviderRateIsolationIntegrationTest, EndToEndPipelineRateIsolation) {
  ProviderRegistry registry;
  registry.SetRateLimiter(rate_limiter_);

  // Setup mock client factory
  registry.SetClientFactory([](const ProviderConfig& cfg) {
    return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
  });

  // Register Provider 1 with low limit: 2 RPM
  ProviderConfig p1;
  p1.name = "limited_cloud";
  p1.endpoint = "https://cloud.ai/v1/chat/completions";
  p1.default_model = "cloud-fast";
  p1.rate_limits.push_back(RateLimitRule{
      .provider = "limited_cloud",
      .window_name = "minute",
      .capacity = 2.0,
      .refill_rate_per_sec = 2.0 / 60.0,
      .ttl_seconds = 60,
  });
  EXPECT_TRUE(registry.RegisterProvider(p1));

  // Register Provider 2 with high limit: 50 RPM
  ProviderConfig p2;
  p2.name = "unlimited_local";
  p2.endpoint = "http://localhost:11434/v1/chat/completions";
  p2.default_model = "local-llama";
  p2.rate_limits.push_back(RateLimitRule{
      .provider = "unlimited_local",
      .window_name = "minute",
      .capacity = 50.0,
      .refill_rate_per_sec = 50.0 / 60.0,
      .ttl_seconds = 60,
  });
  EXPECT_TRUE(registry.RegisterProvider(p2));

  auto model_router = ModelRouter::CreateFromRegistry(registry);
  auto pipeline = std::make_shared<RateLimitedBackend>(model_router, rate_limiter_, "limited_cloud");

  pipeline->SetProviderResolver([model_router](std::string_view model) -> std::string {
    std::string remapped;
    auto cascade = model_router->ResolveCascade(model, &remapped);
    if (cascade && !cascade->Targets().empty()) {
      return std::string(cascade->Targets()[0].client->ProviderName());
    }
    return "";
  });

  ChatCompletionRequest req_cloud;
  req_cloud.model = "cloud-fast";
  req_cloud.messages = {ChatMessage{.role = "user", .content = "Hello Cloud"}};

  ChatCompletionRequest req_local;
  req_local.model = "local-llama";
  req_local.messages = {ChatMessage{.role = "user", .content = "Hello Local"}};

  // 1st and 2nd requests to cloud-fast succeed
  ChatCompletionResponse resp1;
  Status st1 = pipeline->Complete(req_cloud, &resp1);
  EXPECT_TRUE(st1.ok());
  EXPECT_FALSE(resp1.choices.empty());
  EXPECT_EQ(resp1.model, "cloud-fast");

  ChatCompletionResponse resp2;
  Status st2 = pipeline->Complete(req_cloud, &resp2);
  EXPECT_TRUE(st2.ok());
  EXPECT_FALSE(resp2.choices.empty());
  EXPECT_EQ(resp2.model, "cloud-fast");

  // 3rd request to cloud-fast returns StatusCode::RESOURCE_EXHAUSTED (429 Rate Limited)
  ChatCompletionResponse resp3;
  Status st3 = pipeline->Complete(req_cloud, &resp3);
  EXPECT_FALSE(st3.ok());
  EXPECT_EQ(st3.code, StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(st3.message.find("Rate limit exceeded"), std::string::npos);

  // Requests to local-llama must succeed immediately despite cloud-fast being throttled
  for (int i = 0; i < 5; ++i) {
    ChatCompletionResponse resp_loc;
    Status st_loc = pipeline->Complete(req_local, &resp_loc);
    EXPECT_TRUE(st_loc.ok());
    EXPECT_FALSE(resp_loc.choices.empty());
    EXPECT_EQ(resp_loc.model, "local-llama");
  }
}

TEST_F(ProviderRateIsolationIntegrationTest, ThreeTierConcurrentBurstQuotaFailover) {
  // Tier 1 = 10 RPM, Tier 2 = 15 RPM, Tier 3 = 25 RPM => 50 cumulative RPM across cascade
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "burst_t1",
      .window_name = "minute",
      .capacity = 10.0,
      .refill_rate_per_sec = 0.0, // zero refill during burst window so caps stay exact
      .ttl_seconds = 60,
  });
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "burst_t2",
      .window_name = "minute",
      .capacity = 15.0,
      .refill_rate_per_sec = 0.0,
      .ttl_seconds = 60,
  });
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "burst_t3",
      .window_name = "minute",
      .capacity = 25.0,
      .refill_rate_per_sec = 0.0,
      .ttl_seconds = 60,
  });

  CircuitBreakerConfig b_cfg;
  b_cfg.consecutive_failure_threshold = 3;
  b_cfg.base_cooldown_ms = 500;
  b_cfg.max_cooldown_ms = 2000;
  b_cfg.trip_on_429 = true;

  auto t1_client = std::make_shared<MockUpstreamClient>("burst_t1", "m1", "http://t1");
  auto t2_client = std::make_shared<MockUpstreamClient>("burst_t2", "m2", "http://t2");
  auto t3_client = std::make_shared<MockUpstreamClient>("burst_t3", "m3", "http://t3");
  t1_client->SetDefaultResponseText("Served by T1");
  t2_client->SetDefaultResponseText("Served by T2");
  t3_client->SetDefaultResponseText("Served by T3");

  auto t1_breaker = std::make_shared<CircuitBreaker>(b_cfg);
  auto t2_breaker = std::make_shared<CircuitBreaker>(b_cfg);
  auto t3_breaker = std::make_shared<CircuitBreaker>(b_cfg);

  std::vector<CascadeTarget> targets = {
      CascadeTarget{.client = t1_client, .breaker = t1_breaker, .target_model = "m1", .priority = 0},
      CascadeTarget{.client = t2_client, .breaker = t2_breaker, .target_model = "m2", .priority = 1},
      CascadeTarget{.client = t3_client, .breaker = t3_breaker, .target_model = "m3", .priority = 2},
  };

  auto cascade = std::make_shared<CascadeRouter>(targets, "3-tier-burst", rate_limiter_);

  constexpr size_t kConcurrentRequests = 50;
  std::barrier start_gate(static_cast<ptrdiff_t>(kConcurrentRequests));
  std::vector<std::thread> threads;
  threads.reserve(kConcurrentRequests);

  std::atomic<size_t> successes{0};
  std::atomic<size_t> failures{0};

  for (size_t i = 0; i < kConcurrentRequests; ++i) {
    threads.emplace_back([&, i]() {
      start_gate.arrive_and_wait();
      ChatCompletionRequest req;
      req.correlation_id = "burst-" + std::to_string(i);
      req.messages.push_back(ChatMessage{.role = "user", .content = "burst query " + std::to_string(i)});
      req.no_cache = true;

      ChatCompletionResponse resp;
      Status st = cascade->Complete(req, &resp);
      if (st.ok() && !resp.choices.empty()) {
        successes.fetch_add(1, std::memory_order_relaxed);
      } else {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // Zero dropped requests across combined 50-request cumulative capacity
  EXPECT_EQ(successes.load(), 50u);
  EXPECT_EQ(failures.load(), 0u);

  // Strict per-tier quota compliance (zero overrun!)
  EXPECT_EQ(t1_client->CallCount(), 10u);
  EXPECT_EQ(t2_client->CallCount(), 15u);
  EXPECT_EQ(t3_client->CallCount(), 25u);

  // Local throttling must NEVER trip upstream outage circuit breakers
  EXPECT_EQ(t1_breaker->GetState(), CircuitState::CLOSED);
  EXPECT_EQ(t2_breaker->GetState(), CircuitState::CLOSED);
  EXPECT_EQ(t3_breaker->GetState(), CircuitState::CLOSED);

  // 51st request when ALL 3 tiers are at 0 remaining quota -> RESOURCE_EXHAUSTED
  ChatCompletionRequest req51;
  req51.correlation_id = "burst-51";
  req51.messages.push_back(ChatMessage{.role = "user", .content = "overflow"});
  ChatCompletionResponse resp51;
  Status st51 = cascade->Complete(req51, &resp51);
  EXPECT_FALSE(st51.ok());
  EXPECT_EQ(st51.code, StatusCode::RESOURCE_EXHAUSTED);
}
