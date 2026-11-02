#pragma once

#include "tessera/common/types.hpp"
#include "tessera/cache/redis_pool.hpp"
#include <memory>
#include <optional>
#include <chrono>

namespace tessera {

struct CacheLookupResult {
  bool hit{false};
  ChatCompletionResponse response;
  std::string cache_tier{"exact"};
  double latency_ms{0.0};
};

class ExactCacheService {
 public:
  explicit ExactCacheService(
      std::shared_ptr<RedisConnectionPool> redis_pool,
      uint32_t default_ttl_seconds = 86400);

  // Look up exact match in Redis
  CacheLookupResult Lookup(const ChatCompletionRequest& req);

  // Store completion in Redis with TTL
  bool Store(const ChatCompletionRequest& req, const ChatCompletionResponse& resp, uint32_t ttl_seconds = 0);

  // Serialization helpers
  static std::string SerializeResponse(const ChatCompletionResponse& resp);
  static std::optional<ChatCompletionResponse> DeserializeResponse(std::string_view json_str);

  void SetDefaultTTL(uint32_t ttl_sec) noexcept { default_ttl_seconds_ = ttl_sec; }
  [[nodiscard]] uint32_t GetDefaultTTL() const noexcept { return default_ttl_seconds_; }

 private:
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  uint32_t default_ttl_seconds_{86400};
};

}  // namespace tessera

