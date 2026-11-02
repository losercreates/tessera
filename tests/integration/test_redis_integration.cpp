#include <gtest/gtest.h>
#include "tessera/cache/cached_backend.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace tessera;

TEST(RedisIntegrationTest, SubTwoMillisecondExactCacheHits) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig config;
  config.min_connections = 4;
  config.max_connections = 16;
  auto pool = std::make_shared<RedisConnectionPool>(config, mock_redis);
  auto cache_service = std::make_shared<ExactCacheService>(pool, 86400);
  auto inner_backend = std::make_shared<MockEchoBackend>();
  auto cached_backend = std::make_shared<CachedCompletionBackend>(inner_backend, cache_service);

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Benchmark prompt for exact match caching."});

  // 1. Initial query primes cache (MISS)
  ChatCompletionResponse resp_initial;
  Status st = cached_backend->Complete(req, &resp_initial);
  ASSERT_TRUE(st.ok());
  EXPECT_FALSE(resp_initial.cache_hit);

  // 2. Measure 1,000 consecutive cache hits
  constexpr size_t kIterations = 1000;
  std::vector<double> latencies_ms;
  latencies_ms.reserve(kIterations);

  for (size_t i = 0; i < kIterations; ++i) {
    ChatCompletionResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    Status st_hit = cached_backend->Complete(req, &resp);
    auto t1 = std::chrono::steady_clock::now();

    ASSERT_TRUE(st_hit.ok());
    EXPECT_TRUE(resp.cache_hit);
    EXPECT_EQ(resp.cache_tier, "exact");
    EXPECT_EQ(resp.choices[0].message.content, resp_initial.choices[0].message.content);

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    latencies_ms.push_back(ms);
  }

  std::sort(latencies_ms.begin(), latencies_ms.end());
  double p50 = latencies_ms[static_cast<size_t>(kIterations * 0.50)];
  double p95 = latencies_ms[static_cast<size_t>(kIterations * 0.95)];
  double p99 = latencies_ms[static_cast<size_t>(kIterations * 0.99)];

  // Success Criteria: p95 latency < 2.0ms
  EXPECT_LT(p95, 2.0);
  EXPECT_LT(p50, 1.0);
  EXPECT_LT(p99, 5.0);
}
