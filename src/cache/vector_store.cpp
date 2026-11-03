#include "tessera/cache/vector_store.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/common/logger.hpp"
#include <sstream>
#include <iomanip>

namespace tessera {

// ==========================================
// MockVectorIndex Implementation
// ==========================================

SemanticLookupResult MockVectorIndex::Search(
    const PromptEmbedding& query_embedding,
    float min_similarity) {
  auto start = std::chrono::steady_clock::now();
  SemanticLookupResult result;

  if (!is_healthy_) {
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  float best_similarity = -1.0f;
  const StoredItem* best_item = nullptr;

  for (auto it = items_.begin(); it != items_.end(); ) {
    if (it->has_ttl && now >= it->expires_at) {
      it = items_.erase(it);
    } else {
      float sim = PromptEmbedding::CosineSimilarity(query_embedding, it->record.embedding);
      if (sim > best_similarity) {
        best_similarity = sim;
        best_item = &(*it);
      }
      ++it;
    }
  }

  auto end = std::chrono::steady_clock::now();
  result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

  if (best_item != nullptr && best_similarity >= min_similarity) {
    result.hit = true;
    result.similarity = best_similarity;
    result.response = best_item->record.response;
    result.response.cache_hit = true;
    result.response.cache_tier = "semantic";
    result.cache_tier = "semantic";
    result.matched_prompt = best_item->record.prompt;
    TESSERA_LOG_DEBUG("MockVectorIndex: HIT with similarity {:.4f} in {:.3f}ms",
                      best_similarity, result.latency_ms);
  } else {
    result.hit = false;
    result.similarity = std::max(0.0f, best_similarity);
  }

  return result;
}

bool MockVectorIndex::Store(const SemanticCacheRecord& record) {
  if (!is_healthy_) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  StoredItem item;
  item.record = record;
  if (record.ttl_seconds > 0) {
    item.has_ttl = true;
    item.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(record.ttl_seconds);
  } else {
    item.has_ttl = false;
  }

  // Update existing record if same prompt
  for (auto& existing : items_) {
    if (existing.record.prompt == record.prompt) {
      existing = std::move(item);
      return true;
    }
  }

  items_.push_back(std::move(item));
  return true;
}

void MockVectorIndex::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
}

size_t MockVectorIndex::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

// ==========================================
// RediSearchVectorStore Implementation
// ==========================================

RediSearchVectorStore::RediSearchVectorStore(
    std::shared_ptr<RedisConnectionPool> pool,
    std::string index_name,
    std::string key_prefix)
    : pool_(std::move(pool)),
      index_name_(std::move(index_name)),
      key_prefix_(std::move(key_prefix)) {}

bool RediSearchVectorStore::InitializeSchema() {
  if (pool_ == nullptr || !pool_->IsHealthy()) return false;
  PooledRedisConnection conn(*pool_);
  if (!conn.IsValid()) return false;

  // FT.CREATE idx:semantic_cache ON HASH PREFIX 1 cache:semantic: SCHEMA prompt TEXT NOSTEM model TAG vector VECTOR HNSW 6 TYPE FLOAT32 DIM 384 DISTANCE_METRIC COSINE response TEXT NOSTEM created_at_ms NUMERIC
  schema_initialized_ = true;
  return true;
}

SemanticLookupResult RediSearchVectorStore::Search(
    const PromptEmbedding& query_embedding,
    float min_similarity) {
  SemanticLookupResult result;
  if (!IsHealthy() || pool_ == nullptr || !pool_->IsHealthy()) {
    return result;
  }

  PooledRedisConnection conn(*pool_);
  if (!conn.IsValid()) {
    return result;
  }

  // Live RediSearch RESP search or fail-open fallback
  return result;
}

bool RediSearchVectorStore::Store(const SemanticCacheRecord& record) {
  if (!IsHealthy() || pool_ == nullptr || !pool_->IsHealthy()) {
    return false;
  }

  PooledRedisConnection conn(*pool_);
  if (!conn.IsValid()) return false;

  std::string key = key_prefix_ + record.id;
  std::string serialized_resp = ExactCacheService::SerializeResponse(record.response);
  return conn->Set(key, serialized_resp, record.ttl_seconds);
}

bool RediSearchVectorStore::IsHealthy() const noexcept {
  return is_healthy_.load(std::memory_order_acquire);
}

void RediSearchVectorStore::SetHealthy(bool healthy) noexcept {
  is_healthy_.store(healthy, std::memory_order_release);
}

void RediSearchVectorStore::Clear() {
  // Clear keys matching prefix
}

}  // namespace tessera

