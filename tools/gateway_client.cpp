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
#include "tessera/telemetry/telemetry_registry.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  bool check_health = false;
  bool show_metrics = false;
  std::string user_msg = "Hello, gateway!";
  std::string model = "llama-3.1-8b-instant";
  std::string fallback_model = "openrouter/free";
  bool no_cache = false;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;
  float similarity_threshold = 0.95f;
  bool failover_demo = false;
  std::string groq_key;
  std::string openrouter_key;

  if (const char* k = std::getenv("GROQ_API_KEY")) groq_key = k;
  if (const char* k = std::getenv("OPENROUTER_API_KEY")) openrouter_key = k;
  if (const char* m = std::getenv("GROQ_MODEL")) model = m;
  if (const char* fm = std::getenv("OPENROUTER_MODEL")) fallback_model = fm;

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
    } else if (arg.rfind("--message=", 0) == 0) {
      user_msg = arg.substr(10);
    } else if (arg == "--message" && i + 1 < argc) {
      user_msg = argv[++i];
    } else if (arg.rfind("--model=", 0) == 0) {
      model = arg.substr(8);
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

  std::shared_ptr<tessera::IUpstreamClient> groq_client;
  if (!groq_key.empty() && !failover_demo) {
    tessera::UpstreamProviderConfig groq_cfg;
    groq_cfg.name = "groq";
    groq_cfg.default_model = model;
    groq_cfg.endpoint = "https://api.groq.com/openai/v1/chat/completions";
    groq_cfg.api_key = groq_key;
    groq_client = std::make_shared<tessera::HttpUpstreamClient>(groq_cfg);
  } else {
    auto mock_groq = std::make_shared<tessera::MockUpstreamClient>(
        "groq", model, "https://api.groq.com/openai/v1/chat/completions");
    mock_groq->SetDefaultResponseText("Groq completion for: " + user_msg);
    if (failover_demo) {
      mock_groq->EnqueueStatusCode(429, "Simulated Groq 429 quota exhaustion");
    }
    groq_client = mock_groq;
  }

  std::shared_ptr<tessera::IUpstreamClient> openrouter_client;
  if (!openrouter_key.empty()) {
    tessera::UpstreamProviderConfig openrouter_cfg;
    openrouter_cfg.name = "openrouter";
    openrouter_cfg.default_model = fallback_model;
    openrouter_cfg.endpoint = "https://openrouter.ai/api/v1/chat/completions";
    openrouter_cfg.api_key = openrouter_key;
    openrouter_client = std::make_shared<tessera::HttpUpstreamClient>(openrouter_cfg);
  } else {
    auto mock_or = std::make_shared<tessera::MockUpstreamClient>(
        "openrouter", fallback_model, "https://openrouter.ai/api/v1/chat/completions");
    mock_or->SetDefaultResponseText("OpenRouter fallback completion for: " + user_msg);
    openrouter_client = mock_or;
  }

  auto failover_router = std::make_shared<tessera::FailoverRouter>(
      groq_client, groq_breaker, openrouter_client, openrouter_breaker, fallback_model);

  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(failover_router, two_tier_cache);
  auto rate_limiter = std::make_shared<tessera::DistributedRateLimiter>(redis_pool);
  auto pipeline = std::make_shared<tessera::RateLimitedBackend>(cached_backend, rate_limiter, "groq");

  tessera::GatewayServer server(config, pipeline);
  server.Start();

  if (check_health) {
    tessera::HealthStatus status = server.CheckHealth();
    std::cout << "Health Check: " << tessera::HealthStatusToString(status) << "\n";
    server.Stop();
    return status == tessera::HealthStatus::SERVING ? 0 : 1;
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

  std::cout << "\n--- ChatCompletion Response ---\n";
  std::cout << "ID:             " << resp.id << "\n";
  std::cout << "Model:          " << resp.model << "\n";
  std::cout << "Correlation ID: " << resp.correlation_id << "\n";
  std::cout << "Upstream Mode:  " << (!groq_key.empty() ? "LIVE HTTPS (Real Groq API)" : "MOCK (Offline simulated)") << "\n";
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
  auto rstats = failover_router->GetStats();
  std::cout << "Primary Calls:  " << rstats.primary_attempts << " (success=" << rstats.primary_successes << ")\n";
  std::cout << "Failover Calls: " << rstats.failover_attempts << " (direct=" << rstats.direct_fallback_routes << ")\n";
  std::cout << "Groq Circuit:   " << tessera::ToString(groq_breaker->GetState()) << "\n";
  std::cout << "---------------------------------\n";

  if (show_metrics) {
    std::cout << "\n# --- Prometheus Metrics Exposition ---\n";
    std::cout << tessera::TelemetryRegistry::Instance().ToPrometheusFormat();
    std::cout << "# -------------------------------------\n";
  }

  return 0;
}
