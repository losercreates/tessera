#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace tessera {

struct RateLimitRule {
  std::string provider;          // "groq", "openrouter"
  std::string window_name;       // "minute", "day"
  double capacity{30.0};
  double refill_rate_per_sec{0.5};
  uint32_t ttl_seconds{60};

  [[nodiscard]] std::string RedisKey() const {
    return "ratelimit:" + provider + ":" + window_name;
  }
};

struct RateLimitResult {
  bool allowed{true};
  uint32_t remaining_tokens{0};
  uint64_t retry_after_ms{0};
  std::string provider;
};

class RateLimitPolicies {
 public:
  // Groq standard quota: 30 RPM, 1,000 RPD
  static std::vector<RateLimitRule> GroqRules() {
    return {
        RateLimitRule{.provider = "groq", .window_name = "minute", .capacity = 30.0, .refill_rate_per_sec = 30.0 / 60.0, .ttl_seconds = 60},
        RateLimitRule{.provider = "groq", .window_name = "day", .capacity = 1000.0, .refill_rate_per_sec = 1000.0 / 86400.0, .ttl_seconds = 86400}
    };
  }

  // OpenRouter standard quota: 20 RPM, 50 RPD
  static std::vector<RateLimitRule> OpenRouterRules() {
    return {
        RateLimitRule{.provider = "openrouter", .window_name = "minute", .capacity = 20.0, .refill_rate_per_sec = 20.0 / 60.0, .ttl_seconds = 60},
        RateLimitRule{.provider = "openrouter", .window_name = "day", .capacity = 50.0, .refill_rate_per_sec = 50.0 / 86400.0, .ttl_seconds = 86400}
    };
  }
};

}  // namespace tessera

