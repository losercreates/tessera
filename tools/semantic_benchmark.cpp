#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/cache/cached_backend.hpp"
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <iomanip>

int main(int argc, char** argv) {
  size_t iterations = 1000;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--iterations=", 0) == 0) {
      iterations = static_cast<size_t>(std::stoul(arg.substr(13)));
    }
  }

  auto mock_redis = std::make_shared<tessera::MockRedisClient>();
  tessera::RedisPoolConfig pool_cfg;
  auto redis_pool = std::make_shared<tessera::RedisConnectionPool>(pool_cfg, mock_redis);
  auto exact_cache = std::make_shared<tessera::ExactCacheService>(redis_pool, 86400);

  auto embedding_engine = std::make_shared<tessera::MockEmbeddingEngine>();
  auto vector_store = std::make_shared<tessera::MockVectorIndex>();
  auto semantic_cache = std::make_shared<tessera::SemanticCacheService>(embedding_engine, vector_store, 0.95f, 86400);

  auto two_tier_cache = std::make_shared<tessera::TwoTierCacheService>(exact_cache, semantic_cache);
  auto mock_backend = std::make_shared<tessera::MockEchoBackend>();
  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(mock_backend, two_tier_cache);

  // 1. Prime cache with base prompt
  tessera::ChatCompletionRequest prime_req;
  prime_req.model = "llama-3.3-70b-versatile";
  prime_req.messages.push_back(tessera::ChatMessage{
      .role = "user",
      .content = "What is the capital city of France and why is Paris notable?"
  });

  tessera::ChatCompletionResponse prime_resp;
  tessera::Status st = cached_backend->Complete(prime_req, &prime_resp);
  if (!st.ok()) {
    std::cerr << "Failed to prime cache: " << st.message << "\n";
    return 1;
  }

  // Pre-seed paraphrase cluster in semantic store
  tessera::PromptEmbedding para_emb = embedding_engine->GenerateEmbedding(
      "Can you tell me what France's capital city is and why Paris is notable?");
  tessera::SemanticCacheRecord rec;
  rec.id = "benchmark-para-rec";
  rec.prompt = "Can you tell me what France's capital city is and why Paris is notable?";
  rec.model = prime_req.model;
  rec.embedding = para_emb;
  rec.response = prime_resp;
  vector_store->Store(rec);

  tessera::ChatCompletionRequest para_req;
  para_req.model = "llama-3.3-70b-versatile";
  para_req.messages.push_back(tessera::ChatMessage{
      .role = "user",
      .content = "Can you tell me what France's capital city is and why Paris is notable?"
  });

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Tier 2 Semantic Cache Benchmark               \n";
  std::cout << " Iterations: " << iterations << "\n";
  std::cout << " Target SLA: p95 < 20.0ms                              \n";
  std::cout << "=======================================================\n";

  std::vector<double> latencies_ms;
  latencies_ms.reserve(iterations);

  auto total_start = std::chrono::steady_clock::now();

  for (size_t i = 0; i < iterations; ++i) {
    tessera::ChatCompletionResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    tessera::Status s = cached_backend->Complete(para_req, &resp);
    auto t1 = std::chrono::steady_clock::now();

    if (!s.ok() || !resp.cache_hit || resp.cache_tier != "semantic") {
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
  std::cout << "\n--- Semantic Cache Benchmark Results ---\n";
  std::cout << "Total Time:       " << total_time_sec << " s\n";
  std::cout << "Throughput:       " << throughput << " req/sec\n";
  std::cout << "Min Latency:      " << min_ms << " ms\n";
  std::cout << "Avg Latency:      " << avg_ms << " ms\n";
  std::cout << "Max Latency:      " << max_ms << " ms\n";
  std::cout << "p50 Latency:      " << p50_ms << " ms\n";
  std::cout << "p95 Latency:      " << p95_ms << " ms (Target: < 20.0ms)\n";
  std::cout << "p99 Latency:      " << p99_ms << " ms\n";
  std::cout << "----------------------------------------\n";

  if (p95_ms < 20.0) {
    std::cout << ">> PASSED: Tier 2 semantic p95 latency is well within < 20.0ms SLA target!\n\n";
    return 0;
  } else {
    std::cerr << ">> FAILED: p95 latency " << p95_ms << "ms exceeds 20.0ms threshold.\n\n";
    return 1;
  }
}

