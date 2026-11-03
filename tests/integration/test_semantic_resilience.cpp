#include <gtest/gtest.h>
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/cache/cached_backend.hpp"

using namespace tessera;

TEST(SemanticResilienceTest, FailOpenWhenVectorStoreFails) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pool_cfg;
  auto redis_pool = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis);
  auto exact_cache = std::make_shared<ExactCacheService>(redis_pool, 86400);

  auto embedding_engine = std::make_shared<MockEmbeddingEngine>();
  auto vector_store = std::make_shared<MockVectorIndex>();
  auto semantic_cache = std::make_shared<SemanticCacheService>(embedding_engine, vector_store, 0.95f, 86400);

  auto two_tier_cache = std::make_shared<TwoTierCacheService>(exact_cache, semantic_cache);
  auto inner_backend = std::make_shared<MockEchoBackend>();
  auto cached_backend = std::make_shared<CachedCompletionBackend>(inner_backend, two_tier_cache);

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Resilience vector prompt"});

  // Prime cache
  ChatCompletionResponse resp1;
  ASSERT_TRUE(cached_backend->Complete(req, &resp1).ok());

  // Simulate Vector Store Outage
  vector_store->SetHealthy(false);

  // Gateway MUST fail-open: requests continue to succeed via upstream backend with 0 dropped calls
  constexpr size_t kRequestsDuringOutage = 20;
  for (size_t i = 0; i < kRequestsDuringOutage; ++i) {
    ChatCompletionResponse resp;
    Status st = cached_backend->Complete(req, &resp);
    EXPECT_TRUE(st.ok());
  }

  // Restore Vector Store
  vector_store->SetHealthy(true);
  ChatCompletionResponse resp_recovered;
  EXPECT_TRUE(cached_backend->Complete(req, &resp_recovered).ok());
}

