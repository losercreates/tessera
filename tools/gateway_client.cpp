#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  bool check_health = false;
  std::string user_msg = "Hello, gateway!";
  std::string model = "llama-3.3-70b-versatile";
  bool no_cache = false;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--health") {
      check_health = true;
    } else if (arg.rfind("--message=", 0) == 0) {
      user_msg = arg.substr(10);
    } else if (arg == "--message" && i + 1 < argc) {
      user_msg = argv[++i];
    } else if (arg.rfind("--model=", 0) == 0) {
      model = arg.substr(8);
    } else if (arg == "--no-cache" || arg == "--no_cache") {
      no_cache = true;
    } else if (arg.rfind("--redis-host=", 0) == 0) {
      redis_host = arg.substr(13);
    } else if (arg.rfind("--redis-port=", 0) == 0) {
      redis_port = static_cast<uint16_t>(std::stoi(arg.substr(13)));
    } else if (arg.rfind("--cache-ttl=", 0) == 0) {
      cache_ttl = static_cast<uint32_t>(std::stoi(arg.substr(12)));
    }
  }

  tessera::ServerConfig config;
  tessera::RedisPoolConfig pool_config;
  pool_config.host = redis_host;
  pool_config.port = redis_port;
  pool_config.min_connections = 2;
  pool_config.max_connections = 4;

  auto redis_pool = std::make_shared<tessera::RedisConnectionPool>(pool_config);
  auto base_backend = std::make_shared<tessera::MockEchoBackend>();
  auto cache_service = std::make_shared<tessera::ExactCacheService>(redis_pool, cache_ttl);
  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(base_backend, cache_service);
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
  std::cout << "---------------------------------\n";

  return 0;
}
