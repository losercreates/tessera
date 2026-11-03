#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

TwoTierCacheService::TwoTierCacheService(
    std::shared_ptr<ExactCacheService> exact_cache,
    std::shared_ptr<SemanticCacheService> semantic_cache)
    : exact_cache_(std::move(exact_cache)),
      semantic_cache_(std::move(semantic_cache)) {}

TwoTierLookupResult TwoTierCacheService::Lookup(const ChatCompletionRequest& request) {
  TwoTierLookupResult result;

  // 1. Tier 1: SHA-256 Exact-Match Lookup (< 2ms)
  if (exact_cache_ != nullptr) {
    auto exact_res = exact_cache_->Lookup(request);
    if (exact_res.hit) {
      tier1_hits_.fetch_add(1, std::memory_order_relaxed);
      result.hit = true;
      result.cache_tier = "exact";
      result.similarity = 1.0f;
      result.latency_ms = exact_res.latency_ms;
      result.response = std::move(exact_res.response);
      TESSERA_LOG_DEBUG("TwoTierCache: Tier 1 EXACT HIT in {:.3f}ms", result.latency_ms);
      return result;
    }
  }

  // 2. Tier 2: Semantic Vector Cosine Lookup (< 20ms)
  if (semantic_cache_ != nullptr) {
    auto sem_res = semantic_cache_->Lookup(request);
    if (sem_res.hit) {
      tier2_hits_.fetch_add(1, std::memory_order_relaxed);
      result.hit = true;
      result.cache_tier = "semantic";
      result.similarity = sem_res.similarity;
      result.latency_ms = sem_res.latency_ms;
      result.response = std::move(sem_res.response);
      TESSERA_LOG_DEBUG("TwoTierCache: Tier 2 SEMANTIC HIT (Similarity: {:.4f}) in {:.3f}ms",
                       result.similarity, result.latency_ms);
      return result;
    }
  }

  // 3. Cache Miss on Both Tiers
  total_misses_.fetch_add(1, std::memory_order_relaxed);
  result.hit = false;
  return result;
}

bool TwoTierCacheService::Store(
    const ChatCompletionRequest& request,
    const ChatCompletionResponse& response,
    uint32_t ttl_sec) {
  bool exact_stored = false;
  bool semantic_stored = false;

  if (exact_cache_ != nullptr) {
    exact_stored = exact_cache_->Store(request, response, ttl_sec);
  }

  if (semantic_cache_ != nullptr) {
    semantic_stored = semantic_cache_->Store(request, response, ttl_sec);
  }

  return exact_stored || semantic_stored;
}

}  // namespace tessera

