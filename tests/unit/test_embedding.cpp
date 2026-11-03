#include <gtest/gtest.h>
#include "tessera/cache/embedding_engine.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include <sys/resource.h>

using namespace tessera;

TEST(PromptEmbeddingTest, L2UnitNormalization) {
  PromptEmbedding emb;
  for (size_t i = 0; i < emb.values.size(); ++i) {
    emb.values[i] = static_cast<float>(i + 1);
  }

  EXPECT_FALSE(emb.IsNormalized());
  emb.Normalize();
  EXPECT_TRUE(emb.IsNormalized());
  EXPECT_NEAR(emb.Norm(), 1.0f, 1e-4f);
}

TEST(PromptEmbeddingTest, BinaryBlobSerializationRoundtrip) {
  PromptEmbedding orig;
  for (size_t i = 0; i < orig.values.size(); ++i) {
    orig.values[i] = static_cast<float>(static_cast<double>(i) * 0.123);
  }
  orig.Normalize();

  std::vector<uint8_t> blob = orig.ToBinaryBlob();
  EXPECT_EQ(blob.size(), PromptEmbedding::kDimension * sizeof(float));

  PromptEmbedding restored = PromptEmbedding::FromBinaryBlob(blob);
  EXPECT_EQ(restored.values.size(), orig.values.size());
  for (size_t i = 0; i < PromptEmbedding::kDimension; ++i) {
    EXPECT_FLOAT_EQ(restored.values[i], orig.values[i]);
  }
}

TEST(EmbeddingEngineTest, BoundedMemoryUnder1000Queries) {
  MockEmbeddingEngine engine;

  // Track initial memory
  struct rusage usage_before;
  getrusage(RUSAGE_SELF, &usage_before);

  constexpr size_t kQueries = 1000;
  for (size_t i = 0; i < kQueries; ++i) {
    std::string text = "Query prompt variation number " + std::to_string(i) + " for semantic embedding test.";
    PromptEmbedding emb = engine.GenerateEmbedding(text);
    ASSERT_EQ(emb.Dimension(), 384u);
    ASSERT_TRUE(emb.IsNormalized());
  }

  struct rusage usage_after;
  getrusage(RUSAGE_SELF, &usage_after);

  // Maximum RSS in kilobytes
  long max_rss_kb = usage_after.ru_maxrss;
  long max_rss_mb = max_rss_kb / 1024;

  // Principle III Constraint: strictly < 500MB
  EXPECT_LT(max_rss_mb, 500);
}
