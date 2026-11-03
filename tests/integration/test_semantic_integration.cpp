#include <gtest/gtest.h>
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/cache/cached_backend.hpp"
#include <chrono>
#include <vector>
#include <algorithm>

using namespace tessera;

TEST(SemanticIntegrationTest, SubTwentyMillisecondSemanticHits) {
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

  ChatCompletionRequest base_req;
  base_req.model = "llama-3.3-70b-versatile";
  base_req.messages.push_back(ChatMessage{.role = "user", .content = "What is the capital of France?"});

  // 1. Prime cache
  ChatCompletionResponse base_resp;
  ASSERT_TRUE(cached_backend->Complete(base_req, &base_resp).ok());

  // Seed semantic entry for paraphrased variation
  PromptEmbedding para_emb = embedding_engine->GenerateEmbedding("Can you tell me the capital city of France?");
  SemanticCacheRecord rec;
  rec.id = "integration-para-rec";
  rec.prompt = "Can you tell me the capital city of France?";
  rec.model = base_req.model;
  rec.embedding = para_emb;
  rec.response = base_resp;
  vector_store->Store(rec);

  ChatCompletionRequest para_req;
  para_req.model = "llama-3.3-70b-versatile";
  para_req.messages.push_back(ChatMessage{.role = "user", .content = "Can you tell me the capital city of France?"});

  // 2. Measure 100 semantic hits
  constexpr size_t kIterations = 100;
  std::vector<double> latencies_ms;
  latencies_ms.reserve(kIterations);

  for (size_t i = 0; i < kIterations; ++i) {
    ChatCompletionResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    Status st = cached_backend->Complete(para_req, &resp);
    auto t1 = std::chrono::steady_clock::now();

    ASSERT_TRUE(st.ok());
    // Since exact prompt is not in exact cache, it MUST hit semantic cache
    EXPECT_TRUE(resp.cache_hit);
    EXPECT_EQ(resp.cache_tier, "semantic");

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    latencies_ms.push_back(ms);
  }

  std::sort(latencies_ms.begin(), latencies_ms.end());
  double p50 = latencies_ms[static_cast<size_t>(kIterations * 0.50)];
  double p95 = latencies_ms[static_cast<size_t>(kIterations * 0.95)];

  // Success Criteria SC-001: p95 latency < 20.0ms
  EXPECT_LT(p95, 20.0);
  EXPECT_LT(p50, 10.0);
}

