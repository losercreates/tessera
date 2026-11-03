#pragma once

#include "tessera/cache/embedding_engine.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/common/types.hpp"
#include <memory>
#include <string>

namespace tessera {

class SemanticCacheService {
 public:
  SemanticCacheService(
      std::shared_ptr<IEmbeddingEngine> embedding_engine,
      std::shared_ptr<ISemanticCacheStore> vector_store,
      float min_similarity_threshold = 0.95f,
      uint32_t default_ttl_seconds = 86400);

  // Look up semantic match using local query embedding
  SemanticLookupResult Lookup(const ChatCompletionRequest& request);

  // Store completion with its prompt embedding
  bool Store(const ChatCompletionRequest& request, const ChatCompletionResponse& response, uint32_t ttl_sec = 0);

  void SetMinSimilarityThreshold(float threshold) noexcept { min_similarity_threshold_ = threshold; }
  [[nodiscard]] float GetMinSimilarityThreshold() const noexcept { return min_similarity_threshold_; }

  [[nodiscard]] bool IsHealthy() const noexcept {
    return vector_store_ != nullptr && vector_store_->IsHealthy();
  }

  static std::string ExtractCanonicalPrompt(const ChatCompletionRequest& request);

 private:
  std::shared_ptr<IEmbeddingEngine> embedding_engine_;
  std::shared_ptr<ISemanticCacheStore> vector_store_;
  float min_similarity_threshold_{0.95f};
  uint32_t default_ttl_seconds_{86400};
};

}  // namespace tessera

