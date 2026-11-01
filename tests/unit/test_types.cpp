#include <gtest/gtest.h>
#include "tessera/common/types.hpp"
#include "tessera/common/correlation.hpp"

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

