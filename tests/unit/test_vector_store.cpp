#include <gtest/gtest.h>
#include "tessera/cache/vector_store.hpp"
#include <thread>
#include <chrono>

using namespace tessera;

TEST(VectorStoreTest, CosineSimilarityCalculation) {
  PromptEmbedding v1;
  v1.values[0] = 1.0f;
  v1.Normalize();

  PromptEmbedding v2;
  v2.values[0] = 1.0f;
  v2.Normalize();

  // Identical vectors: similarity = 1.0, distance = 0.0
  EXPECT_NEAR(PromptEmbedding::CosineSimilarity(v1, v2), 1.0f, 1e-4f);
  EXPECT_NEAR(PromptEmbedding::CosineDistance(v1, v2), 0.0f, 1e-4f);

  // Orthogonal vectors: similarity = 0.0, distance = 1.0
  PromptEmbedding v3;
  v3.values[1] = 1.0f;
  v3.Normalize();

  EXPECT_NEAR(PromptEmbedding::CosineSimilarity(v1, v3), 0.0f, 1e-4f);
  EXPECT_NEAR(PromptEmbedding::CosineDistance(v1, v3), 1.0f, 1e-4f);
}

TEST(VectorStoreTest, StrictThresholdingAtBoundary) {
  MockVectorIndex store;

  PromptEmbedding base;
  base.values[0] = 1.0f;
  base.Normalize();

  SemanticCacheRecord record;
  record.id = "rec-1";
  record.prompt = "Base prompt";
  record.model = "llama-3.3-70b";
  record.embedding = base;
  record.response.id = "chatcmpl-test";
  record.response.choices.push_back(Choice{
      .index = 0,
      .message = ChatMessage{.role = "assistant", .content = "Response content"}
  });
  store.Store(record);

  // 1. High similarity vector (0.96 > 0.95): HIT
  PromptEmbedding high_sim;
  high_sim.values[0] = 0.96f;
  high_sim.values[1] = std::sqrt(1.0f - 0.96f * 0.96f);
  high_sim.Normalize();

  auto hit_res = store.Search(high_sim, 0.95f);
  EXPECT_TRUE(hit_res.hit);
  EXPECT_GE(hit_res.similarity, 0.95f);
  EXPECT_EQ(hit_res.response.id, "chatcmpl-test");

  // 2. Sub-threshold vector (0.94 < 0.95): MISS
  PromptEmbedding low_sim;
  low_sim.values[0] = 0.94f;
  low_sim.values[1] = std::sqrt(1.0f - 0.94f * 0.94f);
  low_sim.Normalize();

  auto miss_res = store.Search(low_sim, 0.95f);
  EXPECT_FALSE(miss_res.hit);
  EXPECT_LT(miss_res.similarity, 0.95f);
}

TEST(VectorStoreTest, TtlExpirationEviction) {
  MockVectorIndex store;

  PromptEmbedding emb;
  emb.values[0] = 1.0f;
  emb.Normalize();

  SemanticCacheRecord record;
  record.id = "rec-ttl";
  record.prompt = "Expiring record";
  record.embedding = emb;
  record.ttl_seconds = 1; // 1 second TTL
  store.Store(record);

  // Immediate query: HIT
  auto res1 = store.Search(emb, 0.95f);
  EXPECT_TRUE(res1.hit);

  // Sleep past TTL
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Query after TTL: MISS
  auto res2 = store.Search(emb, 0.95f);
  EXPECT_FALSE(res2.hit);
}

