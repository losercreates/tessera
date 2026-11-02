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
  tessera::ServerConfig config;
  std::string redis_host = "127.0.0.1";
  uint16_t redis_port = 6379;
  uint32_t cache_ttl = 86400;
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

  // Milestone 2 Middleware Pipeline:
  // Base MockEchoBackend -> CachedCompletionBackend -> RateLimitedBackend
  auto base_backend = std::make_shared<tessera::MockEchoBackend>();
  auto cache_service = std::make_shared<tessera::ExactCacheService>(redis_pool, cache_ttl);
  auto cached_backend = std::make_shared<tessera::CachedCompletionBackend>(base_backend, cache_service);
  auto rate_limiter = std::make_shared<tessera::DistributedRateLimiter>(redis_pool);
  auto pipeline = std::make_shared<tessera::RateLimitedBackend>(cached_backend, rate_limiter, "groq");

  tessera::GatewayServer server(config, pipeline);
  tessera::GatewayServer::RegisterSignalHandler(&server);

  if (!server.Start()) {
    TESSERA_LOG_ERROR("Failed to start gateway server on port {}", config.port);
    return 1;
  }

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Distributed LLM API Gateway (Milestone 2)     \n";
  std::cout << " Status:    READY & SERVING                            \n";
  std::cout << " Port:      " << config.port << "                      \n";
  std::cout << " Threads:   " << config.worker_threads << "            \n";
  std::cout << " Redis:     " << redis_host << ":" << redis_port << "  \n";
  std::cout << " Cache TTL: " << cache_ttl << "s                       \n";
  std::cout << " Pipeline:  RateLimitedBackend -> CachedBackend -> Mock\n";
  std::cout << "=======================================================\n\n";

  server.Wait();
  return 0;
}
