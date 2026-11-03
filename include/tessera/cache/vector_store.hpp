#pragma once

#include "tessera/cache/embedding_engine.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/common/types.hpp"
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <optional>
#include <atomic>
#include <chrono>

namespace tessera {

struct SemanticCacheRecord {
  std::string id;
  std::string prompt;
  std::string model;
  PromptEmbedding embedding;
  ChatCompletionResponse response;
  int64_t created_at_ms{0};
  uint32_t ttl_seconds{86400};
};

struct SemanticLookupResult {
  bool hit{false};
  float similarity{0.0f};
  double latency_ms{0.0};
  std::string cache_tier{"semantic"};
  ChatCompletionResponse response;
  std::string matched_prompt;
};

class ISemanticCacheStore {
 public:
  virtual ~ISemanticCacheStore() = default;

  virtual SemanticLookupResult Search(
      const PromptEmbedding& query_embedding,
      float min_similarity = 0.95f) = 0;

  virtual bool Store(const SemanticCacheRecord& record) = 0;

  [[nodiscard]] virtual bool IsHealthy() const noexcept = 0;
  virtual void SetHealthy(bool healthy) noexcept = 0;
  virtual void Clear() = 0;
};

// Thread-safe in-memory vector index for offline sandbox execution & testing
class MockVectorIndex : public ISemanticCacheStore {
 public:
  MockVectorIndex() = default;

  SemanticLookupResult Search(
      const PromptEmbedding& query_embedding,
      float min_similarity = 0.95f) override;

  bool Store(const SemanticCacheRecord& record) override;

  [[nodiscard]] bool IsHealthy() const noexcept override { return is_healthy_; }
  void SetHealthy(bool healthy) noexcept override { is_healthy_ = healthy; }
  void Clear() override;

  [[nodiscard]] size_t Size() const;

 private:
  struct StoredItem {
    SemanticCacheRecord record;
    std::chrono::steady_clock::time_point expires_at;
    bool has_ttl{false};
  };

  mutable std::mutex mutex_;
  std::vector<StoredItem> items_;
  bool is_healthy_{true};
};

// RediSearch HNSW vector store client speaking RESP protocol
class RediSearchVectorStore : public ISemanticCacheStore {
 public:
  RediSearchVectorStore(
      std::shared_ptr<RedisConnectionPool> pool,
      std::string index_name = "idx:semantic_cache",
      std::string key_prefix = "cache:semantic:");

  bool InitializeSchema();

  SemanticLookupResult Search(
      const PromptEmbedding& query_embedding,
      float min_similarity = 0.95f) override;

  bool Store(const SemanticCacheRecord& record) override;

  [[nodiscard]] bool IsHealthy() const noexcept override;
  void SetHealthy(bool healthy) noexcept override;
  void Clear() override;

 private:
  std::shared_ptr<RedisConnectionPool> pool_;
  std::string index_name_;
  std::string key_prefix_;
  std::atomic<bool> is_healthy_{true};
  std::atomic<bool> schema_initialized_{false};
};

}  // namespace tessera

