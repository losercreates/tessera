#include "tessera/cache/cached_backend.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

CachedCompletionBackend::CachedCompletionBackend(
    std::shared_ptr<ICompletionBackend> inner_backend,
    std::shared_ptr<ExactCacheService> cache_service)
    : inner_backend_(std::move(inner_backend)),
      cache_service_(std::move(cache_service)) {}

Status CachedCompletionBackend::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  // 1. Check if client explicitly requested cache bypass
  if (!request.no_cache && cache_service_ != nullptr) {
    auto lookup = cache_service_->Lookup(request);
    if (lookup.hit) {
      hit_count_.fetch_add(1, std::memory_order_relaxed);
      *response = std::move(lookup.response);
      // Ensure correlation ID is preserved
      if (!request.correlation_id.empty()) {
        response->correlation_id = request.correlation_id;
      }
      return Status::OK();
    }
  }

  miss_count_.fetch_add(1, std::memory_order_relaxed);

  // 2. Cache miss or bypass: forward to inner backend
  if (inner_backend_ == nullptr) {
    return Status::Internal("No inner backend configured in CachedCompletionBackend");
  }

  Status status = inner_backend_->Complete(request, response);
  if (!status.ok()) {
    return status;
  }

  // 3. Cache successful completion for future queries
  if (cache_service_ != nullptr) {
    cache_service_->Store(request, *response);
  }

  return Status::OK();
}

}  // namespace tessera

