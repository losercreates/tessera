#include <gtest/gtest.h>
#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <chrono>

using namespace tessera;

TEST(ValidationIntegrationTest, RejectsInvalidRequestsInSubMillisecond) {
  ServerConfig config;
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  // Missing messages
  ChatCompletionRequest invalid_req;
  invalid_req.model = "llama-3.3-70b";

  auto start = std::chrono::steady_clock::now();
  ChatCompletionResponse resp;
  Status s = server.ProcessRequestSync(invalid_req, &resp);
  auto end = std::chrono::steady_clock::now();

  double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::INVALID_ARGUMENT);
  EXPECT_LT(duration_ms, 2.0);  // Rejection happens in < 2ms (typically < 0.1ms)

  server.Stop();
}

TEST(ValidationIntegrationTest, EnforcesMaxPayloadSizeLimit) {
  ServerConfig config;
  config.max_message_size = 1024;  // 1KB limit for this test
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  ChatCompletionRequest large_req;
  large_req.model = "test-model";
  large_req.messages.push_back(ChatMessage{.role = "user", .content = std::string(2048, 'A')});

  ChatCompletionResponse resp;
  Status s = server.ProcessRequestSync(large_req, &resp);

  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::RESOURCE_EXHAUSTED);

  server.Stop();
}

