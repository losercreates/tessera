#pragma once

#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/common/types.hpp"
#include <memory>
#include <string>
#include <atomic>

namespace tessera {

struct TwoTierLookupResult {
  bool hit{false};
  std::string cache_tier{""};  // "exact" or "semantic"
  float similarity{1.0f};
  double latency_ms{0.0};
  ChatCompletionResponse response;
};

class TwoTierCacheService {
 public:
  TwoTierCacheService(
      std::shared_ptr<ExactCacheService> exact_cache,
      std::shared_ptr<SemanticCacheService> semantic_cache = nullptr);

  TwoTierLookupResult Lookup(const ChatCompletionRequest& request);

  bool Store(
      const ChatCompletionRequest& request,
      const ChatCompletionResponse& response,
      uint32_t ttl_sec = 0);

  [[nodiscard]] std::shared_ptr<ExactCacheService> GetExactCache() const noexcept {
    return exact_cache_;
  }
  [[nodiscard]] std::shared_ptr<SemanticCacheService> GetSemanticCache() const noexcept {
    return semantic_cache_;
  }

  [[nodiscard]] size_t Tier1Hits() const noexcept { return tier1_hits_.load(); }
  [[nodiscard]] size_t Tier2Hits() const noexcept { return tier2_hits_.load(); }
  [[nodiscard]] size_t TotalMisses() const noexcept { return total_misses_.load(); }

 private:
  std::shared_ptr<ExactCacheService> exact_cache_;
  std::shared_ptr<SemanticCacheService> semantic_cache_;

  std::atomic<size_t> tier1_hits_{0};
  std::atomic<size_t> tier2_hits_{0};
  std::atomic<size_t> total_misses_{0};
};

}  // namespace tessera

