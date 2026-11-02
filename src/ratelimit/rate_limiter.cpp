#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

DistributedRateLimiter::DistributedRateLimiter(std::shared_ptr<RedisConnectionPool> redis_pool)
    : redis_pool_(std::move(redis_pool)) {
  LoadStandardPolicies();
}

void DistributedRateLimiter::AddRule(RateLimitRule rule) {
  std::lock_guard<std::mutex> lock(mutex_);
  rules_by_provider_[rule.provider].push_back(std::move(rule));
}

void DistributedRateLimiter::LoadStandardPolicies() {
  for (const auto& r : RateLimitPolicies::GroqRules()) {
    AddRule(r);
  }
  for (const auto& r : RateLimitPolicies::OpenRouterRules()) {
    AddRule(r);
  }
}

RateLimitResult DistributedRateLimiter::CheckAndConsume(const std::string& provider, double tokens_requested) {
  RateLimitResult result;
  result.provider = provider;
  result.allowed = true;

  std::vector<RateLimitRule> rules;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rules_by_provider_.find(provider);
    if (it != rules_by_provider_.end()) {
      rules = it->second;
    }
  }

  if (rules.empty() || redis_pool_ == nullptr || !redis_pool_->IsHealthy()) {
    result.allowed = true;
    return result;
  }

  PooledRedisConnection conn(*redis_pool_);
  if (!conn.IsValid()) {
    // Fail-open policy if connection unavailable
    result.allowed = true;
    return result;
  }

  uint32_t min_remaining = UINT32_MAX;

  for (const auto& rule : rules) {
    std::string key = rule.RedisKey();
    auto lua_res = conn->EvalRateLimitLua(
        key, tokens_requested, rule.capacity, rule.refill_rate_per_sec, rule.ttl_seconds);

    if (lua_res.size() >= 3) {
      bool window_allowed = (lua_res[0] == 1);
      uint32_t remaining = static_cast<uint32_t>(std::max<int64_t>(0, lua_res[1]));
      uint64_t retry_after = static_cast<uint64_t>(std::max<int64_t>(0, lua_res[2]));

      if (remaining < min_remaining) {
        min_remaining = remaining;
      }

      if (!window_allowed) {
        result.allowed = false;
        result.remaining_tokens = remaining;
        result.retry_after_ms = retry_after;
        TESSERA_LOG_WARN("Rate limit exceeded for provider '{}' in window '{}'. Retry after {}ms",
                         provider, rule.window_name, retry_after);
        return result;
      }
    }
  }

  result.allowed = true;
  result.remaining_tokens = min_remaining != UINT32_MAX ? min_remaining : 0;
  result.retry_after_ms = 0;
  return result;
}

}  // namespace tessera
