# Contract: Semantic Vector Cache & Search Protocols

**Feature**: `003-onnx-semantic-cache`  
**Date**: 2026-11-03  
**Status**: Completed  

---

## 1. RediSearch Protocol Contract

### 1.1. Index Creation (`FT.CREATE`)
The gateway issues this command on startup if the index does not already exist.

```text
FT.CREATE idx:semantic_cache ON HASH PREFIX 1 cache:semantic: SCHEMA prompt TEXT NOSTEM model TAG vector VECTOR HNSW 6 TYPE FLOAT32 DIM 384 DISTANCE_METRIC COSINE M 16 EF_CONSTRUCTION 200 response TEXT NOSTEM created_at_ms NUMERIC
```

*Expected Responses*:
* Success: `+OK\r\n`
* Already Exists: `-Index already exists\r\n` (Treated as success / non-fatal)

---

### 1.2. Top-1 K-Nearest Neighbors Vector Search (`FT.SEARCH`)
Queries the index for the single closest embedding vector using Cosine distance.

```text
FT.SEARCH idx:semantic_cache "*=>[KNN 1 @vector $BLOB AS score]" PARAMS 2 BLOB <1536_bytes_binary_vector> SORTBY score ASC RETURN 3 prompt response score DIALECT 2
```

*Response Parsing*:
1. RediSearch returns the count of matching items followed by key-value pairs.
2. `score` represents Cosine Distance $D \in [0, 2]$.
3. Cosine Similarity $S = 1.0 - D$.
4. **Hit Condition**: $S \ge 0.95 \iff D \le 0.05$. If $D > 0.05$, the result is treated as a **CACHE MISS**.

---

### 1.3. Record Insertion (`HSET` + `EXPIRE`)

```text
HSET cache:semantic:<uuid> prompt <text> model <model_name> vector <1536_bytes_binary_vector> response <json_payload> created_at_ms <epoch_ms>
EXPIRE cache:semantic:<uuid> <ttl_seconds>
```

---

## 2. C++ Interface Contracts

### 2.1. `IEmbeddingEngine`
```cpp
namespace tessera {

class IEmbeddingEngine {
 public:
  virtual ~IEmbeddingEngine() = default;

  // Generate normalized 384-dimensional dense vector
  virtual PromptEmbedding GenerateEmbedding(std::string_view prompt) = 0;

  [[nodiscard]] virtual size_t Dimension() const noexcept = 0;
  [[nodiscard]] virtual std::string_view ModelName() const noexcept = 0;
};

}  // namespace tessera
```

### 2.2. `ISemanticCacheStore`
```cpp
namespace tessera {

class ISemanticCacheStore {
 public:
  virtual ~ISemanticCacheStore() = default;

  virtual SemanticLookupResult Search(
      const PromptEmbedding& query_embedding,
      float min_similarity = 0.95f) = 0;

  virtual bool Store(const SemanticCacheRecord& record) = 0;

  [[nodiscard]] virtual bool IsHealthy() const noexcept = 0;
};

}  // namespace tessera
```

### 2.3. Two-Tier Lookup Contract (`TwoTierCacheService`)
```cpp
namespace tessera {

struct CacheTierResult {
  bool hit{false};
  std::string tier;  // "exact" or "semantic"
  float similarity{1.0f};
  double latency_ms{0.0};
  ChatCompletionResponse response;
};

class TwoTierCacheService {
 public:
  TwoTierCacheService(
      std::shared_ptr<ExactCacheService> exact_cache,
      std::shared_ptr<ISemanticCacheStore> semantic_cache,
      std::shared_ptr<IEmbeddingEngine> embedding_engine);

  CacheTierResult Lookup(const ChatCompletionRequest& request);
  bool Store(const ChatCompletionRequest& request, const ChatCompletionResponse& response, uint32_t ttl_sec = 0);
};

}  // namespace tessera
```

