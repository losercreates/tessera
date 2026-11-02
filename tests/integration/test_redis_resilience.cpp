#include <gtest/gtest.h>
#include "tessera/cache/cached_backend.hpp"
#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <thread>
#include <vector>

using namespace tessera;

class RedisResilienceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig config;
    config.acquire_timeout = std::chrono::milliseconds(20);
    pool_ = std::make_shared<RedisConnectionPool>(config, mock_redis_);
    cache_service_ = std::make_shared<ExactCacheService>(pool_);
    limiter_ = std::make_shared<DistributedRateLimiter>(pool_);
    inner_backend_ = std::make_shared<MockEchoBackend>();

    // Layer stack: RateLimitedBackend -> CachedCompletionBackend -> MockEchoBackend
    auto cached = std::make_shared<CachedCompletionBackend>(inner_backend_, cache_service_);
    pipeline_ = std::make_shared<RateLimitedBackend>(cached, limiter_, "groq");
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> pool_;
  std::shared_ptr<ExactCacheService> cache_service_;
  std::shared_ptr<DistributedRateLimiter> limiter_;
  std::shared_ptr<MockEchoBackend> inner_backend_;
  std::shared_ptr<RateLimitedBackend> pipeline_;
};

TEST_F(RedisResilienceTest, NormalOperationCachesSuccessfully) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "What is Tessera?"});

  // First request: Cache MISS -> calls inner backend -> stores in cache
  ChatCompletionResponse resp1;
  Status st1 = pipeline_->Complete(req, &resp1);
  EXPECT_TRUE(st1.ok());
  EXPECT_FALSE(resp1.cache_hit);

  // Second request: Cache HIT
  ChatCompletionResponse resp2;
  Status st2 = pipeline_->Complete(req, &resp2);
  EXPECT_TRUE(st2.ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(resp2.cache_tier, "exact");
}

TEST_F(RedisResilienceTest, FailOpenWhenRedisFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Resilience prompt"});

  // Prime cache
  ChatCompletionResponse resp1;
  EXPECT_TRUE(pipeline_->Complete(req, &resp1).ok());

  // Simulate Redis failure / partition
  mock_redis_->SetSimulateFailure(true);
  mock_redis_->Disconnect();
  pool_->SetHealthy(false);

  // Gateway MUST fail-open: requests still succeed via inner backend with 0 dropped calls
  constexpr size_t kRequestsDuringOutage = 50;
  for (size_t i = 0; i < kRequestsDuringOutage; ++i) {
    ChatCompletionResponse resp;
    Status st = pipeline_->Complete(req, &resp);
    EXPECT_TRUE(st.ok());
    // Since Redis is down, cache lookup fails open to backend
    EXPECT_FALSE(resp.cache_hit);
  }

  // Restore Redis connection
  mock_redis_->SetSimulateFailure(false);
  mock_redis_->Connect();
  pool_->SetHealthy(true);

  // Gateway recovers normal caching and rate limiting
  ChatCompletionResponse resp_recovered;
  Status st_recovered = pipeline_->Complete(req, &resp_recovered);
  EXPECT_TRUE(st_recovered.ok());
}

