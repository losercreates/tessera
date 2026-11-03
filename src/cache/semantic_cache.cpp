#include "tessera/cache/semantic_cache.hpp"
#include "tessera/common/logger.hpp"
#include "tessera/common/correlation.hpp"
#include <chrono>

namespace tessera {

SemanticCacheService::SemanticCacheService(
    std::shared_ptr<IEmbeddingEngine> embedding_engine,
    std::shared_ptr<ISemanticCacheStore> vector_store,
    float min_similarity_threshold,
    uint32_t default_ttl_seconds)
    : embedding_engine_(std::move(embedding_engine)),
      vector_store_(std::move(vector_store)),
      min_similarity_threshold_(min_similarity_threshold),
      default_ttl_seconds_(default_ttl_seconds) {}

std::string SemanticCacheService::ExtractCanonicalPrompt(const ChatCompletionRequest& request) {
  std::string prompt;
  for (const auto& msg : request.messages) {
    if (msg.role == "system" || msg.role == "user") {
      if (!prompt.empty()) prompt += " ";
      prompt += msg.content;
    }
  }
  return prompt;
}

SemanticLookupResult SemanticCacheService::Lookup(const ChatCompletionRequest& request) {
  SemanticLookupResult result;

  if (embedding_engine_ == nullptr || vector_store_ == nullptr || !vector_store_->IsHealthy()) {
    return result;
  }

  std::string prompt = ExtractCanonicalPrompt(request);
  if (prompt.empty()) {
    return result;
  }

  auto start = std::chrono::steady_clock::now();
  PromptEmbedding query_emb = embedding_engine_->GenerateEmbedding(prompt);

  result = vector_store_->Search(query_emb, min_similarity_threshold_);
  auto end = std::chrono::steady_clock::now();
  result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

  if (result.hit) {
    if (!request.correlation_id.empty()) {
      result.response.correlation_id = request.correlation_id;
    }
    result.response.cache_hit = true;
    result.response.cache_tier = "semantic";
    result.cache_tier = "semantic";
    TESSERA_LOG_DEBUG("Tier 2 Semantic Cache HIT for prompt '{}' (Similarity: {:.4f}, Latency: {:.3f}ms)",
                     prompt, result.similarity, result.latency_ms);
  }

  return result;
}

bool SemanticCacheService::Store(
    const ChatCompletionRequest& request,
    const ChatCompletionResponse& response,
    uint32_t ttl_sec) {
  if (embedding_engine_ == nullptr || vector_store_ == nullptr || !vector_store_->IsHealthy()) {
    return false;
  }

  std::string prompt = ExtractCanonicalPrompt(request);
  if (prompt.empty()) return false;

  PromptEmbedding emb = embedding_engine_->GenerateEmbedding(prompt);

  SemanticCacheRecord record;
  record.id = CorrelationId::Generate();
  record.prompt = prompt;
  record.model = request.model;
  record.embedding = std::move(emb);
  record.response = response;
  record.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  record.ttl_seconds = ttl_sec > 0 ? ttl_sec : default_ttl_seconds_;

  return vector_store_->Store(record);
}

}  // namespace tessera

