#include <gtest/gtest.h>
#include "tessera/ratelimit/rate_limiter.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <barrier>

using namespace tessera;

TEST(RateLimitConcurrencyTest, ExactQuotaUnder200Threads) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig config;
  config.min_connections = 8;
  config.max_connections = 32;
  auto pool = std::make_shared<RedisConnectionPool>(config, mock_redis);
  auto limiter = std::make_shared<DistributedRateLimiter>(pool);

  constexpr size_t kNumThreads = 200;
  // Groq allows exactly 30 RPM
  constexpr size_t kExpectedAllowed = 30;

  std::atomic<size_t> allowed_count{0};
  std::atomic<size_t> rejected_count{0};

  std::barrier sync_point(static_cast<ptrdiff_t>(kNumThreads));
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      // Synchronize all threads so they fire at the exact same moment
      sync_point.arrive_and_wait();

      auto res = limiter->CheckAndConsume("groq", 1.0);
      if (res.allowed) {
        allowed_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        rejected_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(allowed_count.load(), kExpectedAllowed);
  EXPECT_EQ(rejected_count.load(), kNumThreads - kExpectedAllowed);
}

TEST(RateLimitConcurrencyTest, MultiProviderConcurrencyUnder200Threads) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig config;
  config.min_connections = 8;
  config.max_connections = 32;
  auto pool = std::make_shared<RedisConnectionPool>(config, mock_redis);
  auto limiter = std::make_shared<DistributedRateLimiter>(pool);

  constexpr size_t kNumThreads = 200;
  // 100 threads for Groq (capacity 30), 100 threads for OpenRouter (capacity 20)
  std::atomic<size_t> groq_allowed{0};
  std::atomic<size_t> openrouter_allowed{0};

  std::barrier sync_point(static_cast<ptrdiff_t>(kNumThreads));
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (size_t i = 0; i < kNumThreads; ++i) {
    bool is_groq = (i % 2 == 0);
    threads.emplace_back([&, is_groq]() {
      sync_point.arrive_and_wait();
      if (is_groq) {
        auto res = limiter->CheckAndConsume("groq", 1.0);
        if (res.allowed) groq_allowed.fetch_add(1, std::memory_order_relaxed);
      } else {
        auto res = limiter->CheckAndConsume("openrouter", 1.0);
        if (res.allowed) openrouter_allowed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(groq_allowed.load(), 30u);
  EXPECT_EQ(openrouter_allowed.load(), 20u);
}

