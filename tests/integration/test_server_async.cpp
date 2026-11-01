#include <gtest/gtest.h>
#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/common/correlation.hpp"
#include <future>

using namespace tessera;

TEST(ServerAsyncIntegrationTest, UnaryCompletionSucceeds) {
  ServerConfig config;
  config.worker_threads = 2;
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.correlation_id = "test-corr-abc";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Ping gateway"});

  std::promise<std::pair<ChatCompletionResponse, Status>> prom;
  auto fut = prom.get_future();

  server.ProcessRequestAsync(req, [&prom](const ChatCompletionResponse& resp, Status status) {
    prom.set_value({resp, status});
  });

  auto [resp, status] = fut.get();
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(resp.model, "llama-3.3-70b");
  EXPECT_EQ(resp.correlation_id, "test-corr-abc");
  EXPECT_FALSE(resp.choices.empty());
  EXPECT_EQ(resp.choices[0].message.role, "assistant");
  EXPECT_EQ(resp.choices[0].message.content, "Echo response from Tessera Gateway: Ping gateway");

  server.Stop();
}

TEST(ServerAsyncIntegrationTest, AutoGeneratesCorrelationIdIfMissing) {
  ServerConfig config;
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Ping gateway without correlation"});

  ChatCompletionResponse resp;
  Status s = server.ProcessRequestSync(req, &resp);

  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(resp.correlation_id.empty());
  EXPECT_TRUE(CorrelationId::IsValid(resp.correlation_id));

  server.Stop();
}
