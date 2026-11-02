#include <gtest/gtest.h>
#include "tessera/cache/cached_backend.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <thread>
#include <chrono>

using namespace tessera;

class CacheBypassTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig config;
    pool_ = std::make_shared<RedisConnectionPool>(config, mock_redis_);
    cache_service_ = std::make_shared<ExactCacheService>(pool_, /*default_ttl=*/86400);
    inner_backend_ = std::make_shared<MockEchoBackend>();
    cached_backend_ = std::make_shared<CachedCompletionBackend>(inner_backend_, cache_service_);
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> pool_;
  std::shared_ptr<ExactCacheService> cache_service_;
  std::shared_ptr<MockEchoBackend> inner_backend_;
  std::shared_ptr<CachedCompletionBackend> cached_backend_;
};

TEST_F(CacheBypassTest, NoCacheFlagBypassesCacheRead) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Bypass test prompt"});
  req.no_cache = false;

  // 1. First call: miss -> populated in cache
  ChatCompletionResponse resp1;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp1).ok());
  EXPECT_FALSE(resp1.cache_hit);
  EXPECT_EQ(cached_backend_->TotalHits(), 0u);
  EXPECT_EQ(cached_backend_->TotalMisses(), 1u);

  // 2. Second call with no_cache = false: HIT
  ChatCompletionResponse resp2;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp2).ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(cached_backend_->TotalHits(), 1u);
  EXPECT_EQ(cached_backend_->TotalMisses(), 1u);

  // 3. Third call with no_cache = true: BYPASSES cache read
  req.no_cache = true;
  ChatCompletionResponse resp3;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp3).ok());
  EXPECT_FALSE(resp3.cache_hit);
  EXPECT_EQ(cached_backend_->TotalHits(), 1u);
  EXPECT_EQ(cached_backend_->TotalMisses(), 2u);
}

TEST_F(CacheBypassTest, TtlExpirationEvictsCachedEntry) {
  // Configure 1 second TTL
  cache_service_->SetDefaultTTL(1);

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "TTL expiration test"});

  // 1. Initial request: populate cache with 1s TTL
  ChatCompletionResponse resp1;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp1).ok());
  EXPECT_FALSE(resp1.cache_hit);

  // 2. Immediate second request: HIT
  ChatCompletionResponse resp2;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp2).ok());
  EXPECT_TRUE(resp2.cache_hit);

  // 3. Sleep 1.1s for TTL to expire
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // 4. Request after TTL: MISS
  ChatCompletionResponse resp3;
  EXPECT_TRUE(cached_backend_->Complete(req, &resp3).ok());
  EXPECT_FALSE(resp3.cache_hit);
}

