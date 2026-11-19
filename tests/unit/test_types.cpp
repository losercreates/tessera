#include <gtest/gtest.h>
#include "tessera/common/types.hpp"
#include "tessera/common/correlation.hpp"
#include "tessera/routing/circuit_types.hpp"

using namespace tessera;

TEST(TypesTest, StatusCodeConversions) {
  EXPECT_EQ(StatusCodeToString(StatusCode::OK), "OK");
  EXPECT_EQ(StatusCodeToString(StatusCode::INVALID_ARGUMENT), "INVALID_ARGUMENT");
  EXPECT_EQ(StatusCodeToString(StatusCode::RESOURCE_EXHAUSTED), "RESOURCE_EXHAUSTED");
}

TEST(TypesTest, HealthStatusConversions) {
  EXPECT_EQ(HealthStatusToString(HealthStatus::SERVING), "SERVING");
  EXPECT_EQ(HealthStatusToString(HealthStatus::NOT_SERVING), "NOT_SERVING");
}

TEST(TypesTest, CorrelationIdGenerationAndValidation) {
  std::string id = CorrelationId::Generate();
  EXPECT_FALSE(id.empty());
  EXPECT_TRUE(CorrelationId::IsValid(id));

  // Verify uniqueness
  std::string id2 = CorrelationId::Generate();
  EXPECT_NE(id, id2);
  EXPECT_TRUE(CorrelationId::IsValid(id2));

  // Invalid UUID
  EXPECT_FALSE(CorrelationId::IsValid("invalid-uuid-string"));
}

TEST(TypesTest, ByteSizeCalculation) {
  ChatCompletionRequest req;
  req.model = "test-model";
  req.correlation_id = "corr-123";
  req.messages.push_back(ChatMessage{.role = "user", .content = "short message"});

  size_t size = req.ByteSize();
  EXPECT_GT(size, 0);
  EXPECT_GE(size, req.model.size() + req.correlation_id.size() + req.messages[0].content.size());
}

TEST(TypesTest, ProviderConfigValidationAndDefaults) {
  ProviderConfig cfg;
  std::string err;
  EXPECT_FALSE(cfg.Validate(&err));
  EXPECT_FALSE(err.empty());

  cfg.name = "valid_provider-1";
  cfg.endpoint = "https://api.openai.com/v1/chat/completions";
  cfg.default_model = "gpt-4o";
  EXPECT_TRUE(cfg.Validate(&err));

  // Invalid name characters
  cfg.name = "invalid provider name!";
  EXPECT_FALSE(cfg.Validate(&err));

  // Invalid endpoint schema
  cfg.name = "valid-name";
  cfg.endpoint = "ftp://localhost:11434";
  EXPECT_FALSE(cfg.Validate(&err));

  // Local detection
  cfg.endpoint = "http://localhost:11434/v1/chat/completions";
  cfg.api_key = "";
  EXPECT_TRUE(cfg.IsLocal());

  cfg.endpoint = "https://api.deepseek.com/v1/chat/completions";
  cfg.api_key = "sk-valid-key";
  EXPECT_FALSE(cfg.IsLocal());
}

TEST(TypesTest, CascadeRouteAndTargetOperations) {
  CascadeRoute route;
  route.route_id = "test-cascade";
  EXPECT_TRUE(route.Empty());
  EXPECT_EQ(route.TargetCount(), 0u);

  CascadeTarget target;
  target.target_model = "llama3.2";
  target.priority = 0;
  route.targets.push_back(target);

  EXPECT_FALSE(route.Empty());
  EXPECT_EQ(route.TargetCount(), 1u);
}


