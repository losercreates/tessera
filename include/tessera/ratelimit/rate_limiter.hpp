#pragma once

#include "tessera/ratelimit/token_bucket.hpp"
#include "tessera/cache/redis_pool.hpp"
#include <memory>
#include <unordered_map>

namespace tessera {

class DistributedRateLimiter {
 public:
  explicit DistributedRateLimiter(
      std::shared_ptr<RedisConnectionPool> redis_pool,
      bool load_standard_policies = false);

  // Add rule policies for a provider
  void AddRule(RateLimitRule rule);
  void LoadStandardPolicies();
  void ClearRules(const std::string& provider);
  void ClearAllRules();

  [[nodiscard]] std::vector<RateLimitRule> GetRules(const std::string& provider) const;
  [[nodiscard]] bool HasRules(const std::string& provider) const;

  // Check rate limit and deduct tokens atomically via Redis Lua
  RateLimitResult CheckAndConsume(const std::string& provider, double tokens_requested = 1.0);

 private:
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::unordered_map<std::string, std::vector<RateLimitRule>> rules_by_provider_;
  mutable std::mutex mutex_;
};

}  // namespace tessera

