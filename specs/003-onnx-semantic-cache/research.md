# Research & Technical Decisions: Local ONNX Semantic Cache & Vector Search

**Feature**: `003-onnx-semantic-cache`  
**Date**: 2026-11-03  
**Status**: Completed  

---

## 1. Technical Decisions

### Decision 1: Local ONNX Runtime C++ CPU Execution vs External Embedding APIs
* **Context**: Constitution Principle III requires local embedded CPU inference for the `all-MiniLM-L6-v2` model (384-dimensional dense vectors) with a memory footprint strictly under 500MB RAM.
* **Evaluated Options**:
  1. *External Embedding API (OpenAI / HuggingFace Inference API)*: Requires outbound HTTP requests per query; adds 50–150ms network roundtrips, external quota exhaustion risks, and per-token API charges. Violates Principle III.
  2. *Embedded ONNX Runtime C++ API*: Standard high-performance C++ inference engine (`onnxruntime_cxx_api.h`). Pre-loads the quantized or FP32 `all-MiniLM-L6-v2.onnx` model (model size ~80MB, runtime memory ~120MB). Inference time on modern x86_64 CPU is ~5–12ms per prompt. Zero external dependencies.
* **Decision**: Adopt embedded **ONNX Runtime C++ CPU execution** (`Ort::Env`, `Ort::Session`). Bundle or mock model weights for offline build environments with a clean C++ abstraction (`IEmbeddingEngine`).

---

### Decision 2: RediSearch HNSW vs FLAT Vector Index Algorithm
* **Context**: RediSearch supports two vector indexing algorithms: `FLAT` (brute-force linear scan) and `HNSW` (Hierarchical Navigable Small World graphs).
* **Comparison**:
  * `FLAT`: Exact nearest neighbors, $O(N)$ complexity. Search latency degrades linearly as cache grows beyond 10,000 entries.
  * `HNSW`: Approximate nearest neighbors, $O(\log N)$ complexity. Provides sub-2ms query times even across 1,000,000 entries, with $> 99.5\%$ recall when `M=16` and `ef_construction=200` are configured.
* **Decision**: Configure `HNSW` vector indexing as primary with `DISTANCE_METRIC COSINE`, `TYPE FLOAT32`, `DIM 384`. Distance metric formula: $\text{Cosine Distance} = 1 - \text{Cosine Similarity}$. Therefore, a similarity threshold of $\ge 0.95$ corresponds to RediSearch distance score $\le 0.05$.

---

### Decision 3: High-Performance C++ WordPiece Tokenizer
* **Context**: `all-MiniLM-L6-v2` requires standard BERT WordPiece tokenization producing three tensors: `input_ids`, `attention_mask`, and `token_type_ids` of type `int64`.
* **Design**:
  * Implement a lightweight, self-contained C++ WordPiece tokenizer (`BertTokenizer`) loading standard vocab tables (`vocab.txt` with ~30,522 tokens).
  * Text normalization: Unicode lowercasing, basic whitespace/punctuation splitting, greedy longest-matching prefix tokenization with `##` subword continuations.
  * Special tokens: Prepend `[CLS]` (token id 101) and append `[SEP]` (token id 102).
  * Tensor shape: `[1, sequence_length]`, padded or truncated to `max_length = 128` (sufficient for 99% of chat completion user prompts) with an attention mask indicating active tokens.

---

### Decision 4: Vector Post-Processing (Mean Pooling & L2 Normalization)
* **Context**: The raw output of `all-MiniLM-L6-v2` is `last_hidden_state` with shape `[1, sequence_length, 384]`.
* **Transformations**:
  1. **Mean Pooling**: Take the weighted average of token embeddings across non-padded tokens (where `attention_mask == 1`).
     $$\vec{v}_{\text{raw}} = \frac{\sum_{i=1}^{L} \text{hidden}[i] \cdot \text{mask}[i]}{\sum_{i=1}^{L} \text{mask}[i]}$$
  2. **L2 Unit Normalization**: Divide vector by its Euclidean norm:
     $$\vec{v} = \frac{\vec{v}_{\text{raw}}}{\|\vec{v}_{\text{raw}}\|_2 + \epsilon}$$
     where $\|\vec{v}\|_2 = \sqrt{\sum v_i^2}$.
* **Benefit**: With unit L2 vectors, cosine similarity simplifies to the Euclidean dot product $\vec{u} \cdot \vec{v}$, dramatically speeding up similarity comparisons.

---

### Decision 5: Two-Tier Cache Architecture (`TwoTierCacheService`)
* **Context**: The gateway needs to coordinate Tier 1 (SHA-256 exact match) and Tier 2 (ONNX semantic match) cleanly without coupling.
* **Workflow**:
  1. **Tier 1 Check**: Compute canonical SHA-256 prompt hash and query Redis `cache:exact:<hash>`.
     - *Hit*: Return immediately ($< 2\text{ms}$), `cache_hit: true, cache_tier: "exact"`. Zero embedding compute.
  2. **Tier 2 Check**: On Tier 1 miss, pass prompt to `IEmbeddingEngine` to compute 384-d normalized vector, then query vector index in Redis Stack via `FT.SEARCH`.
     - *Hit* (Top-1 nearest neighbor with distance $\le 0.05 \implies \text{similarity} \ge 0.95$): Return cached response ($< 20\text{ms}$), `cache_hit: true, cache_tier: "semantic"`.
  3. **Upstream Forward**: On Tier 2 miss, forward request to rate limiter and upstream LLM provider.
  4. **Multi-Tier Indexing**: On successful upstream completion, store the response in:
     - Tier 1: `SETEX cache:exact:<hash> <ttl> <json>`
     - Tier 2: `HSET cache:semantic:<uuid> prompt ... response ... vector <raw_bytes>`

---

### Decision 6: In-Memory Cosine Vector Store Simulation (`MockVectorIndex`)
* **Context**: The gateway test suite must run 100% offline in sandbox builds without requiring an external Redis Stack server.
* **Design**:
  * Implement `MockVectorIndex` maintaining an in-memory list of vectors with raw float dot product evaluation.
  * Supports exact top-1 cosine search, TTL expiration, and insertion.
  * Real `RediSearchClient` speaks native RESP commands (`FT.CREATE`, `FT.SEARCH`) over POSIX sockets against live Redis Stack instances in CI and production.

