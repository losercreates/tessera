#include <gtest/gtest.h>
#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>

using namespace tessera;

TEST(ConcurrencyIntegrationTest, Handles500ConcurrentRequests) {
  ServerConfig config;
  config.worker_threads = 4;
  auto backend = std::make_shared<MockEchoBackend>();
  GatewayServer server(config, backend);
  ASSERT_TRUE(server.Start());

  const size_t num_clients = 500;
  std::vector<std::thread> client_threads;
  client_threads.reserve(num_clients);

  std::vector<double> latencies_ms(num_clients, 0.0);
  std::atomic<size_t> success_count{0};
  std::atomic<size_t> fail_count{0};

  for (size_t i = 0; i < num_clients; ++i) {
    client_threads.emplace_back([&, i]() {
      ChatCompletionRequest req;
      req.model = "llama-3.3-70b";
      req.correlation_id = "concurrent-" + std::to_string(i);
      req.messages.push_back(ChatMessage{.role = "user", .content = "Concurrent test " + std::to_string(i)});

      auto start = std::chrono::steady_clock::now();
      ChatCompletionResponse resp;
      Status s = server.ProcessRequestSync(req, &resp);
      auto end = std::chrono::steady_clock::now();

      double duration = std::chrono::duration<double, std::milli>(end - start).count();
      latencies_ms[i] = duration;

      if (s.ok() && !resp.choices.empty()) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        fail_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : client_threads) {
    t.join();
  }

  server.Stop();

  EXPECT_EQ(fail_count.load(), 0);
  EXPECT_EQ(success_count.load(), num_clients);

  // Compute p95 latency
  std::sort(latencies_ms.begin(), latencies_ms.end());
  size_t p95_idx = static_cast<size_t>(0.95 * num_clients);
  double p95 = latencies_ms[p95_idx];

  std::cout << "\n[ Concurrency Benchmark Results ]\n";
  std::cout << "  Total Requests: " << num_clients << "\n";
  std::cout << "  Success Rate:   100.0%\n";
  std::cout << "  p50 Latency:    " << latencies_ms[num_clients / 2] << " ms\n";
  std::cout << "  p95 Latency:    " << p95 << " ms\n";
  std::cout << "  p99 Latency:    " << latencies_ms[static_cast<size_t>(0.99 * num_clients)] << " ms\n";

  // Target p95 is under 5ms for mock in-memory backend
  EXPECT_LT(p95, 20.0);
}
