#include <gtest/gtest.h>
#include "tessera/backend/mock_backend.hpp"

using namespace tessera;

TEST(MockEchoBackendTest, CompletesValidRequest) {
  MockEchoBackend backend;
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.correlation_id = "test-corr-123";
  req.messages.push_back(ChatMessage{.role = "system", .content = "You are a helpful assistant."});
  req.messages.push_back(ChatMessage{.role = "user", .content = "Hello, gateway!"});

  ChatCompletionResponse resp;
  Status status = backend.Complete(req, &resp);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(resp.model, "llama-3.3-70b");
  EXPECT_EQ(resp.correlation_id, "test-corr-123");
  EXPECT_EQ(resp.choices.size(), 1);
  EXPECT_EQ(resp.choices[0].message.role, "assistant");
  EXPECT_EQ(resp.choices[0].message.content, "Echo response from Tessera Gateway: Hello, gateway!");
  EXPECT_EQ(resp.choices[0].finish_reason, "stop");
  EXPECT_GT(resp.usage.prompt_tokens, 0);
  EXPECT_GT(resp.usage.completion_tokens, 0);
  EXPECT_EQ(resp.usage.total_tokens, resp.usage.prompt_tokens + resp.usage.completion_tokens);
}

TEST(MockEchoBackendTest, FailsOnNullResponse) {
  MockEchoBackend backend;
  ChatCompletionRequest req;
  Status status = backend.Complete(req, nullptr);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
}

TEST(MockEchoBackendTest, SimulatesFailureCorrectly) {
  MockEchoBackend backend;
  backend.SetSimulatedFailure(true, StatusCode::UNAVAILABLE, "Service temporarily down");

  ChatCompletionRequest req;
  req.model = "test-model";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Hi"});

  ChatCompletionResponse resp;
  Status status = backend.Complete(req, &resp);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code, StatusCode::UNAVAILABLE);
  EXPECT_EQ(status.message, "Service temporarily down");
}

