#include <gtest/gtest.h>
#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

using namespace tessera;

TEST(GracefulShutdownTest, DrainsInFlightRequestsCleanly) {
  ServerConfig config;
  config.worker_threads = 4;
  config.graceful_drain_timeout_ms = 3000;

  // Simulate 100ms processing delay in backend
  auto backend = std::make_shared<MockEchoBackend>(std::chrono::milliseconds(100));
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  const int in_flight_count = 20;
  std::vector<std::thread> threads;
  std::atomic<int> completed_count{0};
  std::atomic<int> ok_count{0};

  for (int i = 0; i < in_flight_count; ++i) {
    threads.emplace_back([&, i]() {
      ChatCompletionRequest req;
      req.model = "test-model";
      req.messages.push_back(ChatMessage{.role = "user", .content = "In flight " + std::to_string(i)});

      ChatCompletionResponse resp;
      Status s = server.ProcessRequestSync(req, &resp);
      completed_count.fetch_add(1);
      if (s.ok()) {
        ok_count.fetch_add(1);
      }
    });
  }

  // Allow threads to enter server
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_GT(server.InFlightCount(), 0);

  // Trigger graceful stop
  server.Stop();

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(completed_count.load(), in_flight_count);
  EXPECT_EQ(ok_count.load(), in_flight_count);
  EXPECT_EQ(server.InFlightCount(), 0);
  EXPECT_FALSE(server.IsRunning());
  EXPECT_EQ(server.CheckHealth(), HealthStatus::NOT_SERVING);
}

TEST(GracefulShutdownTest, RejectsNewRequestsWhenStopped) {
  ServerConfig config;
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  server.Start();
  server.Stop();

  ChatCompletionRequest req;
  req.model = "test-model";
  req.messages.push_back(ChatMessage{.role = "user", .content = "After stop"});

  ChatCompletionResponse resp;
  Status s = server.ProcessRequestSync(req, &resp);

  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::UNAVAILABLE);
}

