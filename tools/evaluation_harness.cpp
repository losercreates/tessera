#include "tessera/eval/evaluation_runner.hpp"
#include "tessera/eval/evaluation_dataset.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/routing/failover_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <iostream>
#include <string>
#include <cstdlib>

using namespace tessera;

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog << " [OPTIONS]\n"
            << "Synthetic Evaluation Harness for Tessera Gateway\n\n"
            << "Options:\n"
            << "  --queries=<N>             Total number of queries (default: 1000)\n"
            << "  --threads=<N>             Worker concurrency (default: 8)\n"
            << "  --output-json=<path>      File path to write JSON evaluation metrics report\n"
            << "  --simulated-latency=<ms>  Simulated upstream latency in ms (default: 25)\n"
            << "  --verbose                 Print detailed query execution logs\n"
            << "  --help                    Show this help message\n";
}

int main(int argc, char** argv) {
  EvaluationConfig config;
  config.query_count = 1000;
  config.paraphrase_ratio = 0.30;
  config.concurrency = 8;
  config.verbose = false;

  uint32_t simulated_latency_ms = 25;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--queries=", 0) == 0) {
      config.query_count = static_cast<size_t>(std::stoul(arg.substr(10)));
    } else if (arg.rfind("--threads=", 0) == 0) {
      config.concurrency = static_cast<size_t>(std::stoul(arg.substr(10)));
    } else if (arg.rfind("--output-json=", 0) == 0) {
      config.output_json_path = arg.substr(14);
    } else if (arg.rfind("--simulated-latency=", 0) == 0) {
      simulated_latency_ms = static_cast<uint32_t>(std::stoul(arg.substr(20)));
    } else if (arg == "--verbose") {
      config.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  std::cout << "Initializing Tessera Gateway Evaluation Pipeline...\n";
  std::cout << "Configuration: " << config.query_count << " queries, "
            << config.concurrency << " threads, "
            << static_cast<int>(config.paraphrase_ratio * 100.0) << "% paraphrases, "
            << simulated_latency_ms << "ms simulated upstream latency.\n\n";

  // 1. Setup upstream clients
  auto primary_client = std::make_shared<MockUpstreamClient>(
      "groq", "llama-3.3-70b-versatile", "https://api.groq.com/openai/v1/chat/completions");
  primary_client->SetDefaultResponseText("Synthesized evaluation completion response.");
  primary_client->SetSimulatedLatency(std::chrono::milliseconds(simulated_latency_ms));

  auto fallback_client = std::make_shared<MockUpstreamClient>(
      "openrouter", "google/gemma-2-9b-it", "https://openrouter.ai/api/v1/chat/completions");
  fallback_client->SetDefaultResponseText("Fallback evaluation response.");

  CircuitBreakerConfig cb_cfg;
  cb_cfg.consecutive_failure_threshold = 5;
  auto primary_breaker = std::make_shared<CircuitBreaker>(cb_cfg);
  auto fallback_breaker = std::make_shared<CircuitBreaker>(cb_cfg);

  auto router = std::make_shared<FailoverRouter>(
      primary_client, primary_breaker, fallback_client, fallback_breaker, "google/gemma-2-9b-it");

  // 2. Setup Redis & Two-Tier Cache
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pool_cfg;
  auto redis_pool = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis);
  auto exact_cache = std::make_shared<ExactCacheService>(redis_pool, 86400);

  auto embedding_engine = std::make_shared<MockEmbeddingEngine>();
  auto vector_store = std::make_shared<MockVectorIndex>();
  auto semantic_cache = std::make_shared<SemanticCacheService>(embedding_engine, vector_store, 0.95f, 86400);

  auto two_tier_cache = std::make_shared<TwoTierCacheService>(exact_cache, semantic_cache);
  auto cached_backend = std::make_shared<CachedCompletionBackend>(router, two_tier_cache);

  // 3. Execute benchmark
  EvaluationRunner runner(cached_backend);
  EvaluationReport report = runner.Run(config);

  // 4. Print report
  std::cout << EvaluationRunner::FormatMarkdownReport(report) << "\n";

  if (!config.output_json_path.empty()) {
    std::cout << "Saved JSON metrics report to: " << config.output_json_path << "\n";
  }

  // 5. Verification gates
  bool pass = true;
  if (report.failed_queries > 0) {
    std::cerr << "[FAIL] Failed queries detected: " << report.failed_queries << "\n";
    pass = false;
  }
  if (report.p95_latency_reduction_pct < 80.0) {
    std::cerr << "[FAIL] p95 Latency reduction was " << report.p95_latency_reduction_pct << "% (Target: >= 80.0%)\n";
    pass = false;
  }
  if (report.max_rss_mb >= 500.0) {
    std::cerr << "[FAIL] Resident memory exceeded 500MB: " << report.max_rss_mb << "MB\n";
    pass = false;
  }

  if (pass) {
    std::cout << "All evaluation acceptance criteria and SLA gates PASSED.\n";
    return 0;
  } else {
    std::cerr << "One or more evaluation SLA gates FAILED.\n";
    return 1;
  }
}

