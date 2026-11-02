#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include <memory>

namespace tessera {

class CachedCompletionBackend : public ICompletionBackend {
 public:
  CachedCompletionBackend(
      std::shared_ptr<ICompletionBackend> inner_backend,
      std::shared_ptr<ExactCacheService> cache_service);

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "CachedCompletionBackend";
  }

  [[nodiscard]] size_t TotalHits() const noexcept { return hit_count_.load(); }
  [[nodiscard]] size_t TotalMisses() const noexcept { return miss_count_.load(); }

 private:
  std::shared_ptr<ICompletionBackend> inner_backend_;
  std::shared_ptr<ExactCacheService> cache_service_;

  std::atomic<size_t> hit_count_{0};
  std::atomic<size_t> miss_count_{0};
};

}  // namespace tessera

