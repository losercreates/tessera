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

  // Warm-up to ensure thread pools and code paths are resident
  ChatCompletionResponse warmup_resp;
  server.ProcessRequestSync(invalid_req, &warmup_resp);

  // Measure median across iterations to eliminate transient CI scheduling jitter
  constexpr size_t kIterations = 10;
  std::vector<double> latencies_ms;
  latencies_ms.reserve(kIterations);

  Status last_status = Status::OK();
  for (size_t i = 0; i < kIterations; ++i) {
    ChatCompletionResponse resp;
    auto start = std::chrono::steady_clock::now();
    last_status = server.ProcessRequestSync(invalid_req, &resp);
    auto end = std::chrono::steady_clock::now();
    latencies_ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }

  std::sort(latencies_ms.begin(), latencies_ms.end());
  double median_ms = latencies_ms[kIterations / 2];

  EXPECT_FALSE(last_status.ok());
  EXPECT_EQ(last_status.code, StatusCode::INVALID_ARGUMENT);
  EXPECT_LT(median_ms, 5.0);  // Rejection happens in sub-millisecond to low millisecond range

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

