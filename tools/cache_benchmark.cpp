#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <iomanip>

int main(int argc, char** argv) {
  size_t iterations = 10000;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  bool force_mock = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--iterations=", 0) == 0) {
      iterations = static_cast<size_t>(std::stoul(arg.substr(13)));
    } else if (arg.rfind("--redis-host=", 0) == 0) {
      redis_host = arg.substr(13);
      force_mock = false;
    } else if (arg.rfind("--redis-port=", 0) == 0) {
      redis_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
      force_mock = false;
    } else if (arg == "--real-redis") {
      force_mock = false;
    }
  }

  tessera::RedisPoolConfig pool_config;
  pool_config.host = redis_host;
  pool_config.port = redis_port;
  pool_config.min_connections = 4;
  pool_config.max_connections = 16;

  std::shared_ptr<tessera::IRedisClient> mock_client = nullptr;
  if (force_mock) {
    mock_client = std::make_shared<tessera::MockRedisClient>();
  }

  auto pool = std::make_shared<tessera::RedisConnectionPool>(pool_config, mock_client);
  auto cache_service = std::make_shared<tessera::ExactCacheService>(pool, 86400);
  auto mock_backend = std::make_shared<tessera::MockEchoBackend>();
  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(mock_backend, cache_service);

  tessera::ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(tessera::ChatMessage{
      .role = "user",
      .content = "What is the capital of France and why is Paris known as the City of Light?"
  });

  // 1. Prime cache
  tessera::ChatCompletionResponse prime_resp;
  tessera::Status st = cached_backend->Complete(req, &prime_resp);
  if (!st.ok()) {
    std::cerr << "Failed to prime cache: " << st.message << "\n";
    return 1;
  }

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Exact-Match Cache Benchmark                   \n";
  std::cout << " Iterations: " << iterations << "\n";
  std::cout << " Target:     p95 < 2.0ms                               \n";
  std::cout << "=======================================================\n";

  std::vector<double> latencies_ms;
  latencies_ms.reserve(iterations);

  auto total_start = std::chrono::steady_clock::now();

  for (size_t i = 0; i < iterations; ++i) {
    tessera::ChatCompletionResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    tessera::Status s = cached_backend->Complete(req, &resp);
    auto t1 = std::chrono::steady_clock::now();

    if (!s.ok() || !resp.cache_hit) {
      std::cerr << "Unexpected cache miss or failure at iteration " << i << "\n";
      return 1;
    }

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    latencies_ms.push_back(ms);
  }

  auto total_end = std::chrono::steady_clock::now();
  double total_time_sec = std::chrono::duration<double>(total_end - total_start).count();

  std::sort(latencies_ms.begin(), latencies_ms.end());
  double min_ms = latencies_ms.front();
  double max_ms = latencies_ms.back();
  double sum_ms = std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0);
  double avg_ms = sum_ms / static_cast<double>(iterations);
  double p50_ms = latencies_ms[static_cast<size_t>(static_cast<double>(iterations) * 0.50)];
  double p95_ms = latencies_ms[static_cast<size_t>(static_cast<double>(iterations) * 0.95)];
  double p99_ms = latencies_ms[static_cast<size_t>(static_cast<double>(iterations) * 0.99)];
  double throughput = static_cast<double>(iterations) / total_time_sec;

  std::cout << std::fixed << std::setprecision(4);
  std::cout << "\n--- Benchmark Results ---\n";
  std::cout << "Total Time:       " << total_time_sec << " s\n";
  std::cout << "Throughput:       " << throughput << " req/sec\n";
  std::cout << "Min Latency:      " << min_ms << " ms\n";
  std::cout << "Avg Latency:      " << avg_ms << " ms\n";
  std::cout << "Max Latency:      " << max_ms << " ms\n";
  std::cout << "p50 Latency:      " << p50_ms << " ms\n";
  std::cout << "p95 Latency:      " << p95_ms << " ms (Target: < 2.0ms)\n";
  std::cout << "p99 Latency:      " << p99_ms << " ms\n";
  std::cout << "-------------------------\n";

  if (p95_ms < 2.0) {
    std::cout << ">> PASSED: p95 latency is well within < 2.0ms SLA target!\n\n";
    return 0;
  } else {
    std::cerr << ">> FAILED: p95 latency " << p95_ms << "ms exceeds 2.0ms threshold.\n\n";
    return 1;
  }
}

