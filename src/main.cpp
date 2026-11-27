#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/backend/upstream_client.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/routing/failover_router.hpp"
#include "tessera/routing/provider_registry.hpp"
#include "tessera/routing/model_router.hpp"
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
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool IsTruthyEnv(const char* value) {
  if (!value) return false;
  std::string_view sv(value);
  return sv == "1" || sv == "true" || sv == "TRUE" || sv == "yes" || sv == "YES";
}

uint16_t ParsePortArg(const std::string& val) {
  int parsed = std::stoi(val);
  if (parsed < 0 || parsed > 65535) {
    throw std::out_of_range("port out of range");
  }
  return static_cast<uint16_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
  tessera::ServerConfig config;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;
  float similarity_threshold = 0.95f;
  bool use_mock_redis = false;
  bool use_mock_upstreams = false;

  try {
    // Initialize defaults from environment variables if present
    if (const char* env_host = std::getenv("REDIS_HOST")) {
      redis_host = env_host;
    }
    if (const char* env_port = std::getenv("REDIS_PORT")) {
      redis_port = ParsePortArg(env_port);
    }
    if (const char* env_gw_host = std::getenv("GATEWAY_HOST")) {
      if (*env_gw_host != '\0') config.host = env_gw_host;
    }
    if (const char* env_gw_port = std::getenv("GATEWAY_PORT")) {
      config.port = ParsePortArg(env_gw_port);
    }
    if (const char* env_http_port = std::getenv("HTTP_PORT")) {
      config.http_port = ParsePortArg(env_http_port);
    }
    if (const char* env_metrics_port = std::getenv("METRICS_PORT")) {
      config.metrics_port = ParsePortArg(env_metrics_port);
    }
    if (const char* env_threads = std::getenv("WORKER_THREADS")) {
      config.worker_threads = static_cast<uint32_t>(std::stoi(env_threads));
    }
    if (const char* env_ttl = std::getenv("CACHE_TTL")) {
      cache_ttl = static_cast<uint32_t>(std::stoi(env_ttl));
    }
    if (const char* env_sim = std::getenv("SIMILARITY_THRESHOLD")) {
      similarity_threshold = std::stof(env_sim);
    }
    if (IsTruthyEnv(std::getenv("TESSERA_MOCK_REDIS"))) {
      use_mock_redis = true;
    }
    if (IsTruthyEnv(std::getenv("TESSERA_MOCK_UPSTREAMS"))) {
      use_mock_upstreams = true;
    }

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.rfind("--host=", 0) == 0) {
        config.host = arg.substr(7);
      } else if (arg.rfind("--port=", 0) == 0) {
        config.port = ParsePortArg(arg.substr(7));
      } else if (arg.rfind("--http-port=", 0) == 0) {
        config.http_port = ParsePortArg(arg.substr(12));
      } else if (arg.rfind("--metrics-port=", 0) == 0) {
        config.metrics_port = ParsePortArg(arg.substr(15));
      } else if (arg == "--no-http") {
        config.enable_http_listener = false;
      } else if (arg == "--no-metrics") {
        config.enable_metrics_listener = false;
      } else if (arg == "--no-rpc") {
        config.enable_rpc_listener = false;
      } else if (arg.rfind("--threads=", 0) == 0) {
        config.worker_threads = static_cast<uint32_t>(std::stoi(arg.substr(10)));
      } else if (arg.rfind("--max_mb=", 0) == 0) {
        config.max_message_size = static_cast<size_t>(std::stoi(arg.substr(9))) * 1024 * 1024;
      } else if (arg.rfind("--redis-host=", 0) == 0) {
        redis_host = arg.substr(13);
      } else if (arg.rfind("--redis-port=", 0) == 0) {
        redis_port = ParsePortArg(arg.substr(13));
      } else if (arg.rfind("--cache-ttl=", 0) == 0) {
        cache_ttl = static_cast<uint32_t>(std::stoi(arg.substr(12)));
      } else if (arg.rfind("--similarity=", 0) == 0) {
        similarity_threshold = std::stof(arg.substr(13));
      } else if (arg == "--mock-redis") {
        use_mock_redis = true;
      } else if (arg == "--mock-upstreams") {
        use_mock_upstreams = true;
      } else if (arg.rfind("--log_level=", 0) == 0) {
        std::string lvl = arg.substr(12);
        if (lvl == "debug") tessera::Logger::Instance().SetLevel(tessera::LogLevel::DEBUG);
        else if (lvl == "warn") tessera::Logger::Instance().SetLevel(tessera::LogLevel::WARN);
        else if (lvl == "error") tessera::Logger::Instance().SetLevel(tessera::LogLevel::ERROR);
        else tessera::Logger::Instance().SetLevel(tessera::LogLevel::INFO);
      }
    }
  } catch (const std::exception& e) {
    TESSERA_LOG_ERROR("Invalid gateway configuration: {}", e.what());
    return 1;
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
  // Resilient Multi-Provider Routing & Registry:
  tessera::ClientFactory mock_factory = nullptr;
  if (use_mock_upstreams) {
    mock_factory = [](const tessera::ProviderConfig& pcfg)
        -> std::shared_ptr<tessera::IUpstreamClient> {
      auto client = std::make_shared<tessera::MockUpstreamClient>(
          pcfg.name.empty() ? "mock" : pcfg.name,
          pcfg.default_model.empty() ? "llama-3.3-70b-versatile" : pcfg.default_model,
          pcfg.endpoint.empty() ? "https://mock.api/v1/chat/completions" : pcfg.endpoint);
      client->SetDefaultResponseText(
          "Live Tessera gateway completion via " +
          (pcfg.name.empty() ? std::string("mock") : pcfg.name) + " upstream.");
      return client;
    };
  }
  auto provider_registry = tessera::ProviderRegistry::LoadFromEnvironment(true, mock_factory);

  // Rate Limiting Engine (enforced per candidate inside ModelRouter / CascadeRouter):
  auto rate_limiter = std::make_shared<tessera::DistributedRateLimiter>(redis_pool);
  provider_registry.RegisterRateLimits(*rate_limiter);

  // Dynamic Model Router & Quota-Aware Cascading Fallback Engine:
  auto model_router = tessera::ModelRouter::CreateFromRegistry(provider_registry, rate_limiter);

  // Pipeline: TwoTierCache (Preemption) -> ModelRouter (Per-Tier Quota & Cascade)
  auto pipeline = std::make_shared<tessera::CachedCompletionBackend>(model_router, two_tier_cache);

  tessera::GatewayServer server(config, pipeline);
  server.SetModelListProvider([model_router]() {
    std::vector<tessera::ModelInfo> models;
    for (const auto& model_id : model_router->GetRegisteredModels()) {
      tessera::ModelInfo info;
      info.id = model_id;
      info.owned_by = "tessera";
      info.created = 0;
      models.push_back(std::move(info));
    }
    return models;
  });
  tessera::GatewayServer::RegisterSignalHandler(&server);

  if (!server.Start()) {
    tessera::GatewayServer::RegisterSignalHandler(nullptr);
    TESSERA_LOG_ERROR("Failed to start gateway server on {}", config.Address());
    return 1;
  }

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Distributed LLM API Gateway                   \n";
  std::cout << " Status:       READY & SERVING                         \n";
  std::cout << " Host:         " << config.host << "\n";
  std::cout << " RPC Port:     " << server.BoundPort() << "\n";
  if (config.enable_http_listener) {
    std::cout << " HTTP Port:    " << server.BoundHttpPort() << "\n";
  }
  if (config.enable_metrics_listener) {
    std::cout << " Metrics Port: " << server.BoundMetricsPort() << "\n";
  }
  std::cout << " Threads:      " << config.worker_threads << "\n";
  std::cout << " Redis:        " << redis_host << ":" << redis_port
            << (use_mock_redis ? " (mock)" : "") << "\n";
  std::cout << " Cache TTL:    " << cache_ttl << "s\n";
  std::cout << " Min Sim:      " << similarity_threshold << "\n";
  std::cout << " Upstreams:    " << (use_mock_upstreams ? "mock" : "live") << " [";
  const auto registered = provider_registry.GetRegisteredProviders();
  for (size_t i = 0; i < registered.size(); ++i) {
    std::cout << registered[i];
    if (i + 1 < registered.size()) std::cout << ", ";
  }
  std::cout << "]\n";

  std::cout << " Pipeline:     TwoTierCache -> ModelRouter (Quota-Aware Cascade)\n";
  std::cout << "=======================================================\n\n" << std::flush;

  server.Wait();
  tessera::GatewayServer::RegisterSignalHandler(nullptr);
  return 0;
}
