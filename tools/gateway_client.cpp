#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/upstream_client.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/routing/provider_registry.hpp"
#include "tessera/routing/model_router.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include "tessera/common/logger.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  bool check_health = false;
  bool show_metrics = false;
  bool failover_demo = false;
  bool live_mode = false;
  bool no_cache = false;
  bool model_explicit = false;
  std::string user_msg = "Hello, gateway!";
  std::string model;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;
  float similarity_threshold = 0.95f;
  std::string groq_key;
  std::string openrouter_key;

  // Initialize defaults from environment variables if present
  if (const char* env_host = std::getenv("REDIS_HOST")) {
    redis_host = env_host;
  }
  if (const char* env_port = std::getenv("REDIS_PORT")) {
    redis_port = static_cast<uint16_t>(std::stoi(env_port));
  }
  if (const char* env_ttl = std::getenv("CACHE_TTL")) {
    cache_ttl = static_cast<uint32_t>(std::stoi(env_ttl));
  }
  if (const char* env_sim = std::getenv("SIMILARITY_THRESHOLD")) {
    similarity_threshold = std::stof(env_sim);
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--health") {
      check_health = true;
    } else if (arg == "--metrics") {
      show_metrics = true;
    } else if (arg == "--failover-demo") {
      failover_demo = true;
    } else if (arg == "--live") {
      live_mode = true;
    } else if (arg.rfind("--message=", 0) == 0) {
      user_msg = arg.substr(10);
    } else if (arg == "--message" && i + 1 < argc) {
      user_msg = argv[++i];
    } else if (arg.rfind("--model=", 0) == 0) {
      model = arg.substr(8);
      model_explicit = true;
    } else if (arg == "--no-cache" || arg == "--no_cache") {
      no_cache = true;
    } else if (arg.rfind("--groq-key=", 0) == 0) {
      groq_key = arg.substr(11);
    } else if (arg.rfind("--openrouter-key=", 0) == 0) {
      openrouter_key = arg.substr(17);
    } else if (arg.rfind("--redis-host=", 0) == 0) {
      redis_host = arg.substr(13);
    } else if (arg.rfind("--redis-port=", 0) == 0) {
      redis_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
    } else if (arg.rfind("--cache-ttl=", 0) == 0) {
      cache_ttl = static_cast<uint32_t>(std::stoi(arg.substr(12)));
    } else if (arg.rfind("--similarity=", 0) == 0) {
      similarity_threshold = std::stof(arg.substr(13));
    }
  }

  if (!groq_key.empty()) {
    setenv("GROQ_API_KEY", groq_key.c_str(), 1);
  }
  if (!openrouter_key.empty()) {
    setenv("OPENROUTER_API_KEY", openrouter_key.c_str(), 1);
  }

  tessera::ServerConfig config;
  tessera::RedisPoolConfig pool_config;
  pool_config.host = redis_host;
  pool_config.port = redis_port;
  pool_config.min_connections = 2;
  pool_config.max_connections = 4;

  auto redis_pool = std::make_shared<tessera::RedisConnectionPool>(pool_config);
  auto exact_cache = std::make_shared<tessera::ExactCacheService>(redis_pool, cache_ttl);
  auto embedding_engine = std::make_shared<tessera::MockEmbeddingEngine>();
  auto vector_store = std::make_shared<tessera::MockVectorIndex>();
  auto semantic_cache = std::make_shared<tessera::SemanticCacheService>(
      embedding_engine, vector_store, similarity_threshold, cache_ttl);
  auto two_tier_cache = std::make_shared<tessera::TwoTierCacheService>(exact_cache, semantic_cache);

  auto client_factory = [&](const tessera::ProviderConfig& cfg) -> std::shared_ptr<tessera::IUpstreamClient> {
    if ((!cfg.api_key.empty() || live_mode) && !failover_demo) {
      return std::make_shared<tessera::HttpUpstreamClient>(cfg);
    }
    auto mock_client = std::make_shared<tessera::MockUpstreamClient>(
        cfg.name, cfg.default_model, cfg.endpoint);
    mock_client->SetDefaultResponseText(cfg.name + " completion for: " + user_msg);
    return mock_client;
  };

  auto provider_registry = tessera::ProviderRegistry::LoadFromEnvironment(true, client_factory);
  const auto registered_providers = provider_registry.GetRegisteredProviders();

  if (failover_demo && !registered_providers.empty()) {
    auto primary_mock = std::dynamic_pointer_cast<tessera::MockUpstreamClient>(
        provider_registry.GetProviderClient(registered_providers[0]));
    if (primary_mock) {
      primary_mock->EnqueueStatusCode(
          429, "Simulated " + registered_providers[0] + " 429 quota exhaustion");
    }
    if (registered_providers.size() < 2) {
      TESSERA_LOG_WARN(
          "Failover demo enabled with only {} configured provider ('{}'); no fallback tier available.",
          registered_providers.size(), registered_providers[0]);
    }
  }

  auto rate_limiter = std::make_shared<tessera::DistributedRateLimiter>(redis_pool);
  provider_registry.RegisterRateLimits(*rate_limiter);

  auto model_router = tessera::ModelRouter::CreateFromRegistry(provider_registry, rate_limiter);
  auto pipeline = std::make_shared<tessera::CachedCompletionBackend>(model_router, two_tier_cache);

  tessera::GatewayServer server(config, pipeline);
  server.Start();

  if (check_health) {
    tessera::HealthStatus status = server.CheckHealth();
    std::cout << "Health Check: " << tessera::HealthStatusToString(status) << "\n";
    server.Stop();
    return status == tessera::HealthStatus::SERVING ? 0 : 1;
  }

  if (!model_explicit) {
    if (failover_demo && registered_providers.size() >= 2) {
      model = "auto";
    } else if (!registered_providers.empty()) {
      const auto* first_cfg = provider_registry.GetProviderConfig(registered_providers[0]);
      model = (first_cfg && !first_cfg->default_model.empty()) ? first_cfg->default_model : "auto";
    } else {
      model = "auto";
    }
  }

  tessera::ChatCompletionRequest req;
  req.model = model;
  req.messages.push_back(tessera::ChatMessage{.role = "system", .content = "You are a helpful assistant."});
  req.messages.push_back(tessera::ChatMessage{.role = "user", .content = user_msg});
  req.no_cache = no_cache;

  tessera::ChatCompletionResponse resp;
  tessera::Status s = server.ProcessRequestSync(req, &resp);

  server.Stop();

  if (!s.ok()) {
    std::cerr << "Request failed: [" << tessera::StatusCodeToString(s.code) << "] " << s.message << "\n";
    return 1;
  }

  bool any_live = false;
  if (!failover_demo) {
    for (const auto& prov : registered_providers) {
      const auto* pcfg = provider_registry.GetProviderConfig(prov);
      if (pcfg && (!pcfg->api_key.empty() || live_mode)) {
        any_live = true;
        break;
      }
    }
  }

  std::cout << "\n--- ChatCompletion Response ---\n";
  std::cout << "ID:             " << resp.id << "\n";
  std::cout << "Model:          " << resp.model << "\n";
  std::cout << "Correlation ID: " << resp.correlation_id << "\n";
  std::cout << "Upstream Mode:  " << (any_live ? "LIVE HTTP (Configured endpoints)" : "MOCK (Offline simulated)") << "\n";
  std::cout << "Providers:      [";
  for (size_t i = 0; i < registered_providers.size(); ++i) {
    std::cout << registered_providers[i];
    if (i + 1 < registered_providers.size()) std::cout << ", ";
  }
  std::cout << "]\n";
  std::cout << "Cache Hit:      " << (resp.cache_hit ? "true" : "false") << "\n";
  if (!resp.cache_tier.empty()) {
    std::cout << "Cache Tier:     " << resp.cache_tier << "\n";
  }
  if (!resp.choices.empty()) {
    std::cout << "Role:           " << resp.choices[0].message.role << "\n";
    std::cout << "Content:        " << resp.choices[0].message.content << "\n";
    std::cout << "Finish Reason:  " << resp.choices[0].finish_reason << "\n";
  }
  std::cout << "Prompt Tokens:  " << resp.usage.prompt_tokens << "\n";
  std::cout << "Compl Tokens:   " << resp.usage.completion_tokens << "\n";
  std::cout << "Total Tokens:   " << resp.usage.total_tokens << "\n";
  std::string remapped_dummy;
  auto active_cascade = model_router->ResolveCascade(req.model, &remapped_dummy);
  if (!active_cascade) {
    active_cascade = model_router->GetDefaultCascade();
  }
  if (active_cascade) {
    auto cstats = active_cascade->GetStats();
    std::cout << "Cascade Stats:  route=" << active_cascade->RouteId()
              << ", success=" << cstats.successful_requests
              << ", failovers=" << cstats.failover_attempts
              << ", direct_bypasses=" << cstats.direct_bypasses
              << ", rate_limit_skips=" << cstats.rate_limit_skips << "\n";
  }
  for (const auto& prov : registered_providers) {
    if (auto breaker = provider_registry.GetProviderBreaker(prov)) {
      std::cout << "Circuit [" << prov << "]: " << tessera::ToString(breaker->GetState()) << "\n";
    }
  }
  std::cout << "---------------------------------\n";

  if (show_metrics) {
    std::cout << "\n# --- Prometheus Metrics Exposition ---\n";
    std::cout << tessera::TelemetryRegistry::Instance().ToPrometheusFormat();
    std::cout << "# -------------------------------------\n";
  }

  return 0;
}
