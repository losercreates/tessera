#include <gtest/gtest.h>
#include "tessera/cache/sha256.hpp"

using namespace tessera;

TEST(Sha256Test, HashingKnownVectors) {
  // Empty string SHA-256 is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  std::string empty_hash = Sha256::Hash("");
  EXPECT_EQ(empty_hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "hello" SHA-256
  std::string hello_hash = Sha256::Hash("hello");
  EXPECT_EQ(hello_hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(Sha256Test, CanonicalStringIsDeterministic) {
  ChatCompletionRequest req1;
  req1.model = "llama-3.3-70b";
  req1.temperature = 0.7f;
  req1.messages.push_back(ChatMessage{.role = "system", .content = "You are a helpful assistant."});
  req1.messages.push_back(ChatMessage{.role = "user", .content = "What is distributed consensus?"});

  ChatCompletionRequest req2 = req1;

  std::string s1 = Sha256::ComputeCanonicalString(req1);
  std::string s2 = Sha256::ComputeCanonicalString(req2);
  EXPECT_EQ(s1, s2);

  std::string key1 = Sha256::GetExactCacheKey(req1);
  std::string key2 = Sha256::GetExactCacheKey(req2);
  EXPECT_EQ(key1, key2);
  EXPECT_EQ(key1.rfind("cache:exact:", 0), 0);
  EXPECT_EQ(key1.size(), 12 + 64);  // "cache:exact:" (12) + 64 hex chars
}

TEST(Sha256Test, DifferentPromptsProduceDifferentKeys) {
  ChatCompletionRequest req1;
  req1.model = "llama-3.3-70b";
  req1.messages.push_back(ChatMessage{.role = "user", .content = "Prompt A"});

  ChatCompletionRequest req2;
  req2.model = "llama-3.3-70b";
  req2.messages.push_back(ChatMessage{.role = "user", .content = "Prompt B"});

  EXPECT_NE(Sha256::GetExactCacheKey(req1), Sha256::GetExactCacheKey(req2));
}

