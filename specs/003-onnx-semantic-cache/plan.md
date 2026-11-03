# Implementation Plan: Local ONNX Semantic Cache & Vector Search

**Branch**: `003-onnx-semantic-cache` | **Date**: 2026-11-03 | **Spec**: [specs/003-onnx-semantic-cache/spec.md](spec.md)

**Input**: Feature specification from `specs/003-onnx-semantic-cache/spec.md`

---

## Summary

Implement Tier 2 local semantic caching using an embedded CPU inference engine (`all-MiniLM-L6-v2` generating 384-dimensional dense float vectors) paired with RediSearch vector cosine similarity search. Incoming requests check Tier 1 SHA-256 exact-match cache first (< 2ms); upon a miss, Tier 2 generates normalized embeddings locally and queries RediSearch for top-1 nearest neighbor with cosine similarity $\ge 0.95$ (< 20ms). If both tiers miss, the query proceeds upstream, and the resulting completion is indexed in both tiers. The engine supports `--no-cache` bypass, configurable TTL, and transparent fail-open degradation under vector store outages while maintaining a memory footprint strictly under 500MB.

---

## Technical Context

**Language/Version**: C++20 (GCC 13.3+ / Clang 18.1+)  
**Primary Dependencies**: OpenSSL (`libcrypto`), ONNX Runtime C++ (`onnxruntime`), POSIX Sockets, pthreads  
**Storage**: Redis Stack / RediSearch (HNSW cosine vector index, dim 384) + In-Memory Vector Index Simulation for offline builds  
**Testing**: GoogleTest / `tests/gtest_compat`, CTest, ASan, UBSan  
**Target Platform**: Linux x86_64 (Ubuntu 22.04+)  
**Project Type**: Distributed API Gateway Service & Shared Core Library  
**Performance Goals**: Tier 2 semantic cache retrieval p95 $< 20\text{ms}$; Tier 1 exact cache retrieval p95 $< 2\text{ms}$; Throughput $> 1,000\text{ req/s}$  
**Constraints**: Resident memory for local embedding runtime $< 500\text{MB}$ RAM; L2 vector normalization ($\|V\|_2 = 1.0$); zero dropped requests on fail-open degradation; zero memory leaks or undefined behavior under ASan/UBSan  
**Scale/Scope**: 384-dimensional vectors, 512 token max context length, top-1 nearest neighbor with similarity threshold $\ge 0.95$  

---

## Constitution Check

*GATE: Evaluated before Phase 0 research and re-checked after Phase 1 design.*

| Principle | Requirement | Plan Alignment | Status |
|---|---|---|---|
| **I. Modern C++20** | Strict RAII, `std::unique_ptr`/`std::shared_ptr`, `std::span`, zero raw pointer ownership, async non-blocking concurrency | All embedding models, vector stores, and connections managed by smart pointers and RAII guards; contiguous vector spans used for tensor input buffers. | **PASS** |
| **II. Determinism & Reliability** | Quota compliance, 99.99% availability, fail-open resiliency | If vector search or ONNX model fails, requests fall through to upstream with zero dropped calls. | **PASS** |
| **III. Embedded Semantic Inference** | Local CPU inference (`all-MiniLM-L6-v2`, 384-d), $< 500\text{MB}$ RAM, Two-Tier Cache (< 2ms Tier 1, < 20ms Tier 2, similarity > 0.95) | Embedded ONNX Runtime C++ execution; L2 normalization; RediSearch HNSW cosine index; Tier 1 exact checked before Tier 2 semantic search. | **PASS** |
| **IV. Quality Gate** | $\ge 90\%$ test coverage, 0 ASan/UBSan errors or leaks | Comprehensive unit and integration test suite with synthetic paraphrased queries; AddressSanitizer and UndefinedBehaviorSanitizer enabled. | **PASS** |
| **V. CI/CD Reproducibility** | Automated GitHub Actions CI, clean offline and container builds | Self-contained in-memory cosine vector store for offline sandbox builds; Redis Stack service container in CI matrix. | **PASS** |

---

## Project Structure

### Documentation (this feature)

```text
specs/003-onnx-semantic-cache/
├── spec.md              # Feature specification
├── plan.md              # This plan
├── research.md          # Phase 0 architectural & technical decisions
├── data-model.md        # Phase 1 data entities and RediSearch schemas
├── quickstart.md        # Phase 1 test and verification scenarios
├── contracts/
│   └── semantic_cache_contract.md # Protocol & C++ API contracts
└── checklists/
    └── requirements.md  # Spec quality checklist (PASS)
```

### Source Code Layout

```text
include/tessera/
├── cache/
│   ├── sha256.hpp               # [Existing] SHA-256 prompt hashing
│   ├── redis_client.hpp         # [Existing] Redis client interface & RESP sockets
│   ├── redis_pool.hpp           # [Existing] Redis connection pool
│   ├── exact_cache.hpp          # [Existing] Tier 1 exact-match cache service
│   ├── cached_backend.hpp       # [Existing] Base cache decorator
│   ├── embedding_engine.hpp     # [NEW] IEmbeddingEngine interface & BertTokenizer
│   ├── onnx_embedding.hpp       # [NEW] OnnxEmbeddingEngine & MockEmbeddingEngine
│   ├── vector_store.hpp         # [NEW] ISemanticCacheStore, MockVectorIndex, RediSearchVectorStore
│   ├── semantic_cache.hpp       # [NEW] SemanticCacheService (Tier 2 manager)
│   └── two_tier_cache.hpp       # [NEW] TwoTierCacheService (Tier 1 + Tier 2 coordinator)
└── ratelimit/
    ├── token_bucket.hpp         # [Existing]
    ├── rate_limiter.hpp         # [Existing]
    └── rate_limited_backend.hpp # [Existing]

src/cache/
├── sha256.cpp
├── redis_client.cpp
├── redis_pool.cpp
├── exact_cache.cpp
├── cached_backend.cpp
├── embedding_engine.cpp         # [NEW] Tokenizer & L2 vector normalization math
├── onnx_embedding.cpp           # [NEW] ONNX Runtime session & inference runner
├── vector_store.cpp             # [NEW] RediSearch RESP client & MockVectorIndex
├── semantic_cache.cpp           # [NEW] Semantic vector search & storage logic
└── two_tier_cache.cpp           # [NEW] Two-tier hierarchical cache orchestration

tools/
├── gateway_client.cpp           # CLI client tool
├── cache_benchmark.cpp          # Tier 1 exact cache benchmark
└── semantic_benchmark.cpp       # [NEW] Tier 2 semantic cache benchmark (paraphrased queries)

tests/
├── unit/
│   ├── test_tokenizer.cpp       # [NEW] WordPiece tokenization & padding
│   ├── test_embedding.cpp       # [NEW] 384-d output & L2 unit normalization
│   ├── test_vector_store.cpp    # [NEW] HNSW / Cosine top-1 search & thresholding
│   ├── test_semantic_cache.cpp  # [NEW] Semantic hit/miss & similarity calculation
│   └── test_two_tier_cache.cpp  # [NEW] Tier 1 bypass and Tier 2 fallback flow
└── integration/
    ├── test_semantic_integration.cpp # [NEW] Sub-20ms paraphrased query verification
    └── test_semantic_resilience.cpp  # [NEW] Fail-open behavior when vector store down
```

---

## Phase Execution Breakdown

### Phase 0: Research & Feasibility (Completed)
- Completed `research.md` documenting ONNX Runtime CPU execution, RediSearch HNSW cosine distance math, WordPiece tokenizer, and two-tier coordination.

### Phase 1: Design & Contracts (Completed)
- Completed `data-model.md` defining `PromptEmbedding`, `TokenizedPrompt`, `SemanticCacheRecord`, and RediSearch schema.
- Completed `contracts/semantic_cache_contract.md` defining `FT.CREATE`, `FT.SEARCH`, `IEmbeddingEngine`, `ISemanticCacheStore`, and `TwoTierCacheService`.
- Completed `quickstart.md` defining verification workflows.

### Phase 2: Implementation Sequencing (Upcoming in `/speckit-tasks`)
- Tasks will be generated sequentially to implement tokenizer/embedding foundational components, vector store adapters, two-tier cache coordinator, and benchmark tools with ASan/UBSan verification.
