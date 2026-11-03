#include <gtest/gtest.h>
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/cache/cached_backend.hpp"

using namespace tessera;

class TwoTierCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    redis_pool_ = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis_);
    exact_cache_ = std::make_shared<ExactCacheService>(redis_pool_, 86400);

    embedding_engine_ = std::make_shared<MockEmbeddingEngine>();
    vector_store_ = std::make_shared<MockVectorIndex>();
    semantic_cache_ = std::make_shared<SemanticCacheService>(embedding_engine_, vector_store_, 0.95f, 86400);

    two_tier_cache_ = std::make_shared<TwoTierCacheService>(exact_cache_, semantic_cache_);
    inner_backend_ = std::make_shared<MockEchoBackend>();
    cached_backend_ = std::make_shared<CachedCompletionBackend>(inner_backend_, two_tier_cache_);
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<ExactCacheService> exact_cache_;
  std::shared_ptr<MockEmbeddingEngine> embedding_engine_;
  std::shared_ptr<MockVectorIndex> vector_store_;
  std::shared_ptr<SemanticCacheService> semantic_cache_;
  std::shared_ptr<TwoTierCacheService> two_tier_cache_;
  std::shared_ptr<MockEchoBackend> inner_backend_;
  std::shared_ptr<CachedCompletionBackend> cached_backend_;
};

TEST_F(TwoTierCacheTest, HierarchicalCacheFlow) {
  ChatCompletionRequest req1;
  req1.model = "llama-3.3-70b-versatile";
  req1.messages.push_back(ChatMessage{.role = "user", .content = "What is the capital of France?"});

  // 1. Initial request: MISS on both tiers -> calls inner backend -> populates both tiers
  ChatCompletionResponse resp1;
  Status s1 = cached_backend_->Complete(req1, &resp1);
  ASSERT_TRUE(s1.ok());
  EXPECT_FALSE(resp1.cache_hit);
  EXPECT_EQ(two_tier_cache_->TotalMisses(), 1u);
  EXPECT_EQ(two_tier_cache_->Tier1Hits(), 0u);
  EXPECT_EQ(two_tier_cache_->Tier2Hits(), 0u);

  // 2. Exact duplicate query: Tier 1 EXACT HIT
  ChatCompletionResponse resp2;
  Status s2 = cached_backend_->Complete(req1, &resp2);
  ASSERT_TRUE(s2.ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(resp2.cache_tier, "exact");
  EXPECT_EQ(two_tier_cache_->Tier1Hits(), 1u);
  EXPECT_EQ(two_tier_cache_->Tier2Hits(), 0u);

  // 3. Paraphrased query: Tier 1 MISS, Tier 2 SEMANTIC HIT
  ChatCompletionRequest req_para;
  req_para.model = "llama-3.3-70b-versatile";
  req_para.messages.push_back(ChatMessage{.role = "user", .content = "Tell me what France's capital is."});

  // Seed semantic record for the prompt cluster
  PromptEmbedding emb_para = embedding_engine_->GenerateEmbedding("Tell me what France's capital is.");
  SemanticCacheRecord rec;
  rec.id = "para-rec";
  rec.prompt = "Tell me what France's capital is.";
  rec.model = req_para.model;
  rec.embedding = emb_para;
  rec.response = resp1;
  vector_store_->Store(rec);

  ChatCompletionResponse resp3;
  Status s3 = cached_backend_->Complete(req_para, &resp3);
  ASSERT_TRUE(s3.ok());
  EXPECT_TRUE(resp3.cache_hit);
  EXPECT_EQ(resp3.cache_tier, "semantic");
  EXPECT_EQ(two_tier_cache_->Tier1Hits(), 1u);
  EXPECT_EQ(two_tier_cache_->Tier2Hits(), 1u);

  // 4. Cache bypass with no_cache = true
  req1.no_cache = true;
  ChatCompletionResponse resp4;
  Status s4 = cached_backend_->Complete(req1, &resp4);
  ASSERT_TRUE(s4.ok());
  EXPECT_FALSE(resp4.cache_hit);
}

