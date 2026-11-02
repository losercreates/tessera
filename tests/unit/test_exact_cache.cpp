#include <gtest/gtest.h>
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/backend/mock_backend.hpp"

using namespace tessera;

TEST(ExactCacheTest, SerializationDeserializationRoundtrip) {
  ChatCompletionResponse resp;
  resp.id = "chatcmpl-test-123";
  resp.created = 1730500000;
  resp.model = "llama-3.3-70b";
  resp.correlation_id = "corr-test-456";

  Choice choice;
  choice.index = 0;
  choice.message.role = "assistant";
  choice.message.content = "Serialization works!";
  choice.finish_reason = "stop";
  resp.choices.push_back(choice);

  resp.usage.prompt_tokens = 10;
  resp.usage.completion_tokens = 5;
  resp.usage.total_tokens = 15;

  std::string json = ExactCacheService::SerializeResponse(resp);
  EXPECT_FALSE(json.empty());

  auto deserialized = ExactCacheService::DeserializeResponse(json);
  ASSERT_TRUE(deserialized.has_value());
  EXPECT_EQ(deserialized->id, resp.id);
  EXPECT_EQ(deserialized->model, resp.model);
  EXPECT_EQ(deserialized->correlation_id, resp.correlation_id);
  EXPECT_EQ(deserialized->choices.size(), 1);
  EXPECT_EQ(deserialized->choices[0].message.content, "Serialization works!");
  EXPECT_EQ(deserialized->usage.total_tokens, 15);
}

TEST(ExactCacheTest, CacheMissAndSubsequentHit) {
  RedisPoolConfig cfg;
  auto mock_redis = std::make_shared<MockRedisClient>();
  auto pool = std::make_shared<RedisConnectionPool>(cfg, mock_redis);
  auto cache_service = std::make_shared<ExactCacheService>(pool, 3600);

  auto mock_backend = std::make_shared<MockEchoBackend>();
  CachedCompletionBackend cached_backend(mock_backend, cache_service);

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.correlation_id = "test-corr";
  req.messages.push_back(ChatMessage{.role = "user", .content = "First query"});

  // 1st request: Miss
  ChatCompletionResponse resp1;
  Status s1 = cached_backend.Complete(req, &resp1);
  EXPECT_TRUE(s1.ok());
  EXPECT_FALSE(resp1.cache_hit);
  EXPECT_EQ(cached_backend.TotalMisses(), 1);
  EXPECT_EQ(cached_backend.TotalHits(), 0);

  // 2nd request: Hit
  ChatCompletionResponse resp2;
  Status s2 = cached_backend.Complete(req, &resp2);
  EXPECT_TRUE(s2.ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(resp2.cache_tier, "exact");
  EXPECT_EQ(resp2.choices[0].message.content, resp1.choices[0].message.content);
  EXPECT_EQ(cached_backend.TotalMisses(), 1);
  EXPECT_EQ(cached_backend.TotalHits(), 1);
}

