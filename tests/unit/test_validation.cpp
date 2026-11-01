#include <gtest/gtest.h>
#include "tessera/common/types.hpp"

using namespace tessera;

TEST(ValidationTest, ValidRequestPasses) {
  ChatCompletionRequest req;
  req.model = "gemma-2-9b";
  req.temperature = 0.7f;
  req.messages.push_back(ChatMessage{.role = "system", .content = "System instructions"});
  req.messages.push_back(ChatMessage{.role = "user", .content = "User query"});

  Status s = ValidateRequest(req);
  EXPECT_TRUE(s.ok());
}

TEST(ValidationTest, EmptyModelFails) {
  ChatCompletionRequest req;
  req.messages.push_back(ChatMessage{.role = "user", .content = "User query"});
  Status s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
}

TEST(ValidationTest, EmptyMessagesListFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  Status s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
}

TEST(ValidationTest, EmptyMessageContentFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.messages.push_back(ChatMessage{.role = "user", .content = ""});
  Status s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
}

TEST(ValidationTest, InvalidRoleFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.messages.push_back(ChatMessage{.role = "hacker", .content = "test"});
  Status s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
}

TEST(ValidationTest, InvalidTemperatureFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.temperature = 2.5f;
  req.messages.push_back(ChatMessage{.role = "user", .content = "test"});
  Status s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);

  req.temperature = -0.1f;
  s = ValidateRequest(req);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
}

TEST(ValidationTest, PayloadSizeExceededFails) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.messages.push_back(ChatMessage{.role = "user", .content = std::string(1000, 'X')});

  // Limit to 500 bytes
  Status s = ValidateRequest(req, 500);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::RESOURCE_EXHAUSTED);
}

