#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/backend/upstream_client.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/routing/failover_router.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  tessera::ServerConfig config;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;
  float similarity_threshold = 0.95f;
  bool use_mock_redis = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--port=", 0) == 0) {
      config.port = static_cast<uint16_t>(std::stoi(arg.substr(7)));
    } else if (arg.rfind("--threads=", 0) == 0) {
      config.worker_threads = static_cast<uint32_t>(std::stoi(arg.substr(10)));
    } else if (arg.rfind("--max_mb=", 0) == 0) {
      config.max_message_size = static_cast<size_t>(std::stoi(arg.substr(9))) * 1024 * 1024;
    } else if (arg.rfind("--redis-host=", 0) == 0) {
      redis_host = arg.substr(13);
    } else if (arg.rfind("--redis-port=", 0) == 0) {
      redis_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
    } else if (arg.rfind("--cache-ttl=", 0) == 0) {
      cache_ttl = static_cast<uint32_t>(std::stoi(arg.substr(12)));
    } else if (arg.rfind("--similarity=", 0) == 0) {
      similarity_threshold = std::stof(arg.substr(13));
    } else if (arg == "--mock-redis") {
      use_mock_redis = true;
    } else if (arg.rfind("--log_level=", 0) == 0) {
      std::string lvl = arg.substr(12);
      if (lvl == "debug") tessera::Logger::Instance().SetLevel(tessera::LogLevel::DEBUG);
      else if (lvl == "warn") tessera::Logger::Instance().SetLevel(tessera::LogLevel::WARN);
      else if (lvl == "error") tessera::Logger::Instance().SetLevel(tessera::LogLevel::ERROR);
      else tessera::Logger::Instance().SetLevel(tessera::LogLevel::INFO);
    }
  }

  // Configure Redis connection pool
  tessera::RedisPoolConfig pool_config;
  pool_config.host = redis_host;
  pool_config.port = redis_port;
  pool_config.min_connections = 4;
  pool_config.max_connections = 16;

  std::shared_ptr<tessera::IRedisClient> mock_redis = nullptr;
  if (use_mock_redis) {
    mock_redis = std::make_shared<tessera::MockRedisClient>();
  }
  auto redis_pool = std::make_shared<tessera::RedisConnectionPool>(pool_config, mock_redis);

  // Two-Tier Cache Architecture:
  // Tier 1: SHA-256 Exact-Match Redis Cache (< 2ms)
  // Tier 2: Local ONNX Semantic Vector Cache (< 20ms, similarity >= 0.95)
  auto exact_cache = std::make_shared<tessera::ExactCacheService>(redis_pool, cache_ttl);
  auto embedding_engine = std::make_shared<tessera::MockEmbeddingEngine>();
  auto vector_store = std::make_shared<tessera::MockVectorIndex>();
  auto semantic_cache = std::make_shared<tessera::SemanticCacheService>(
      embedding_engine, vector_store, similarity_threshold, cache_ttl);
  auto two_tier_cache = std::make_shared<tessera::TwoTierCacheService>(exact_cache, semantic_cache);

  // Resilient Upstream Routing & Circuit Breakers:
  // Primary: Groq (Llama 3.3 70B)
  // Fallback: OpenRouter (Gemma-2-9B)
  tessera::CircuitBreakerConfig groq_breaker_cfg;
  groq_breaker_cfg.consecutive_failure_threshold = 3;
  groq_breaker_cfg.base_cooldown_ms = 1000;
  groq_breaker_cfg.max_cooldown_ms = 60000;
  groq_breaker_cfg.trip_on_429 = true;

  tessera::CircuitBreakerConfig openrouter_breaker_cfg;
  openrouter_breaker_cfg.consecutive_failure_threshold = 3;
  openrouter_breaker_cfg.base_cooldown_ms = 1000;
  openrouter_breaker_cfg.max_cooldown_ms = 60000;
  openrouter_breaker_cfg.trip_on_429 = true;

  auto groq_breaker = std::make_shared<tessera::CircuitBreaker>(groq_breaker_cfg);
  auto openrouter_breaker = std::make_shared<tessera::CircuitBreaker>(openrouter_breaker_cfg);

  tessera::UpstreamProviderConfig groq_cfg;
  groq_cfg.name = "groq";
  groq_cfg.default_model = "llama-3.3-70b-versatile";
  groq_cfg.endpoint = "https://api.groq.com/openai/v1/chat/completions";
  auto groq_client = std::make_shared<tessera::HttpUpstreamClient>(groq_cfg);

  tessera::UpstreamProviderConfig openrouter_cfg;
  openrouter_cfg.name = "openrouter";
  openrouter_cfg.default_model = "google/gemma-2-9b-it";
  openrouter_cfg.endpoint = "https://openrouter.ai/api/v1/chat/completions";
  auto openrouter_client = std::make_shared<tessera::HttpUpstreamClient>(openrouter_cfg);

  auto failover_router = std::make_shared<tessera::FailoverRouter>(
      groq_client, groq_breaker, openrouter_client, openrouter_breaker, "google/gemma-2-9b-it");

  // Pipeline: FailoverRouter -> CachedCompletionBackend (Two-Tier) -> RateLimitedBackend
  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(failover_router, two_tier_cache);
  auto rate_limiter = std::make_shared<tessera::DistributedRateLimiter>(redis_pool);
  auto pipeline = std::make_shared<tessera::RateLimitedBackend>(cached_backend, rate_limiter, "groq");

  tessera::GatewayServer server(config, pipeline);
  tessera::GatewayServer::RegisterSignalHandler(&server);

  if (!server.Start()) {
    TESSERA_LOG_ERROR("Failed to start gateway server on port {}", config.port);
    return 1;
  }

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Distributed LLM API Gateway                   \n";
  std::cout << " Status:     READY & SERVING                           \n";
  std::cout << " Port:       " << config.port << "                     \n";
  std::cout << " Threads:    " << config.worker_threads << "           \n";
  std::cout << " Redis:      " << redis_host << ":" << redis_port << "\n";
  std::cout << " Cache TTL:  " << cache_ttl << "s                      \n";
  std::cout << " Min Sim:    " << similarity_threshold << "            \n";
  std::cout << " Upstream:   Groq (primary) -> OpenRouter (fallback)   \n";
  std::cout << " Pipeline:   RateLimit -> TwoTierCache -> FailoverRouter\n";
  std::cout << "=======================================================\n\n";

  server.Wait();
  return 0;
}
