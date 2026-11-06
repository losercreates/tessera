#include "tessera/cache/cached_backend.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

CachedCompletionBackend::CachedCompletionBackend(
    std::shared_ptr<ICompletionBackend> inner_backend,
    std::shared_ptr<ExactCacheService> cache_service)
    : inner_backend_(std::move(inner_backend)),
      exact_cache_(std::move(cache_service)),
      two_tier_cache_(nullptr) {}

CachedCompletionBackend::CachedCompletionBackend(
    std::shared_ptr<ICompletionBackend> inner_backend,
    std::shared_ptr<TwoTierCacheService> two_tier_cache)
    : inner_backend_(std::move(inner_backend)),
      exact_cache_(nullptr),
      two_tier_cache_(std::move(two_tier_cache)) {}

size_t CachedCompletionBackend::Tier1Hits() const noexcept {
  if (two_tier_cache_ != nullptr) return two_tier_cache_->Tier1Hits();
  return hit_count_.load();
}

size_t CachedCompletionBackend::Tier2Hits() const noexcept {
  if (two_tier_cache_ != nullptr) return two_tier_cache_->Tier2Hits();
  return 0;
}

Status CachedCompletionBackend::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  // 1. Check if client explicitly requested cache bypass
  if (!request.no_cache) {
    // A) If TwoTierCache is configured:
    if (two_tier_cache_ != nullptr) {
      auto lookup = two_tier_cache_->Lookup(request);
      if (lookup.hit) {
        hit_count_.fetch_add(1, std::memory_order_relaxed);
        TelemetryRegistry::Instance().IncrementCacheHitsTotal(lookup.cache_tier);
        *response = std::move(lookup.response);
        if (!request.correlation_id.empty()) {
          response->correlation_id = request.correlation_id;
        }
        return Status::OK();
      }
    }
    // B) Or if standalone ExactCache is configured:
    else if (exact_cache_ != nullptr) {
      auto lookup = exact_cache_->Lookup(request);
      if (lookup.hit) {
        hit_count_.fetch_add(1, std::memory_order_relaxed);
        TelemetryRegistry::Instance().IncrementCacheHitsTotal("exact");
        *response = std::move(lookup.response);
        if (!request.correlation_id.empty()) {
          response->correlation_id = request.correlation_id;
        }
        return Status::OK();
      }
    }
  }

  miss_count_.fetch_add(1, std::memory_order_relaxed);
  TelemetryRegistry::Instance().IncrementCacheHitsTotal("miss");

  // 2. Cache miss or bypass: forward to inner backend
  if (inner_backend_ == nullptr) {
    return Status::Internal("No inner backend configured in CachedCompletionBackend");
  }

  Status status = inner_backend_->Complete(request, response);
  if (!status.ok()) {
    return status;
  }

  // 3. Cache successful completion for future queries across active tiers
  if (two_tier_cache_ != nullptr) {
    two_tier_cache_->Store(request, *response);
  } else if (exact_cache_ != nullptr) {
    exact_cache_->Store(request, *response);
  }

  return Status::OK();
}

}  // namespace tessera
