# Feature Specification: Local ONNX Semantic Cache & Vector Search

**Feature Branch**: `003-onnx-semantic-cache`  
**Created**: 2026-11-03  
**Status**: Draft  
**Input**: User description: "Local ONNX Semantic Cache & Vector Search: Embed ONNX Runtime C++ execution for all-MiniLM-L6-v2 (384-dimensional dense embeddings, < 500MB memory footprint), L2 vector normalization, and RediSearch HNSW vector cosine similarity search (similarity threshold > 0.95, p95 < 20ms latency SLA) as a Tier 2 semantic caching fallback behind the Tier 1 exact-match cache."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Semantic Cache Retrieval for Paraphrased Queries (Priority: P1) 🎯 MVP

As an API client or consuming microservice, I want queries that are semantically identical or paraphrased (e.g., "What is the capital of France?" vs "Tell me the capital city of France") to return cached LLM completions without forwarding requests to expensive upstream providers, so that response latency is reduced by up to 90% and provider token costs are preserved.

**Why this priority**: Semantic caching is the core architectural differentiator of the gateway. Exact-match caching alone only captures literal character duplicates; semantic caching unlocks high cache hit ratios for natural language variations in high-traffic microservice environments.

**Independent Test**:
1. Prime the system with an initial query ("What is the capital of France?").
2. Submit a paraphrased variation ("Can you tell me France's capital city?").
3. Verify the response is served from the semantic cache with `cache_hit: true` and `cache_tier: "semantic"`, achieves a measured cosine similarity $> 0.95$, and returns in under 20ms without invoking the upstream LLM backend.

**Acceptance Scenarios**:
1. **Given** an existing completion indexed in the semantic cache with embedding $V_1$,  
   **When** a new request arrives whose normalized embedding $V_2$ has a cosine similarity $\ge 0.95$ with $V_1$,  
   **Then** the gateway immediately returns the cached response with `cache_hit: true` and `cache_tier: "semantic"` in $< 20\text{ms}$ (p95).
2. **Given** an existing completion in the cache,  
   **When** a new request arrives with a semantically distinct topic (cosine similarity $< 0.95$),  
   **Then** the semantic cache reports a miss, the query is forwarded upstream, and the new result is indexed.

---

### User Story 2 - Two-Tier Hierarchical Cache Coordination (Priority: P1)

As a systems operator, I want incoming requests to check the Tier 1 SHA-256 exact cache first (< 2ms) and only evaluate the Tier 2 ONNX semantic vector search (< 20ms) upon a Tier 1 miss, so that computational overhead from vector embedding generation is avoided for exact duplicate queries.

**Why this priority**: Embedding inference has a finite CPU cost (~5–15ms). Submitting all requests to embedding inference would degrade sub-millisecond exact-match lookups. A tiered pipeline preserves the $< 2\text{ms}$ exact SLA while providing a $< 20\text{ms}$ semantic safety net.

**Independent Test**:
1. Submit an identical duplicate prompt; observe instant Tier 1 hit (< 2ms) with zero embedding compute.
2. Submit a paraphrased prompt; observe Tier 1 miss followed immediately by Tier 2 semantic hit (< 20ms).
3. Submit a novel prompt; observe Tier 1 miss, Tier 2 miss, upstream execution, and simultaneous population of both tiers upon completion.

**Acceptance Scenarios**:
1. **Given** a prompt matching an exact previous query,  
   **When** the request is processed,  
   **Then** Tier 1 exact cache returns the response in $< 2\text{ms}$ and Tier 2 embedding generation is bypassed entirely.
2. **Given** a prompt that misses Tier 1 exact cache,  
   **When** Tier 2 evaluates the query,  
   **Then** local embedding generation computes the 384-dimensional vector and executes the similarity search.
3. **Given** a successful upstream completion following a cache miss,  
   **When** response is returned to the client,  
   **Then** the completion is stored in Tier 1 with its SHA-256 key and in Tier 2 with its vector embedding and metadata.

---

### User Story 3 - Local CPU Embedding Inference with Bounded Memory (Priority: P2)

As an infrastructure engineer, I want text embedding inference executed completely locally on CPU without external network calls and bounded to $< 500\text{MB}$ RAM, so that gateway replicas run self-contained without embedding API fees, rate limits, or outbound network vulnerabilities.

**Why this priority**: Relying on an external embedding API introduces external latency, rate limiting, and failure modes that defeat the purpose of local edge caching. Local embedded CPU execution guarantees deterministic latency and self-containment.

**Independent Test**:
1. Run local embedding inference across 1,000 distinct input strings.
2. Verify all outputs are dense 384-dimensional float vectors with unit L2 norm ($\|V\|_2 = 1.0 \pm 10^{-5}$).
3. Monitor process memory to confirm resident set size (RSS) remains strictly below 500MB.

**Acceptance Scenarios**:
1. **Given** an arbitrary UTF-8 input prompt string,  
   **When** passed to the local embedding engine,  
   **Then** it produces a 384-dimensional normalized vector without initiating external network connections.
2. **Given** continuous high-concurrency request processing,  
   **When** monitored over time,  
   **Then** total gateway memory overhead dedicated to the embedding runtime remains $< 500\text{MB}$.

---

### User Story 4 - Semantic Cache Degradation, Selective Bypass & TTL Expiry (Priority: P2)

As an API consumer or developer, I want to bypass semantic cache lookups using `--no-cache`, configure semantic cache TTL, and experience graceful degradation if the vector database or embedding engine is unavailable, so that upstream availability is never compromised.

**Why this priority**: Cache engines must fail open. If the vector index or embedding runtime experiences an internal error, the gateway must transparently fall back to upstream execution with zero dropped client calls.

**Independent Test**:
1. Issue a semantically matching query with `no_cache = true`; verify the cache is bypassed, upstream is invoked, and cache records are refreshed.
2. Simulate vector index unavailability; verify requests transparently route upstream without client-visible errors.
3. Verify that entries older than the configured TTL are evicted and report cache misses.

**Acceptance Scenarios**:
1. **Given** a query that would match an existing semantic cache entry,  
   **When** `no_cache` is set to `true`,  
   **Then** Tier 1 and Tier 2 cache reads are bypassed, a fresh upstream completion is generated, and cache records are updated.
2. **Given** an unavailable or partitioned vector search index,  
   **When** a semantic search lookup is attempted,  
   **Then** the gateway logs a warning and fails open to the upstream provider with zero dropped requests.
3. **Given** an expired semantic cache entry whose TTL has elapsed,  
   **When** a matching query arrives,  
   **Then** the expired entry is treated as a miss.

---

### Edge Cases

- **Borderline Similarity (e.g., 0.949 vs 0.951)**: Queries with similarity score $\ge 0.9500$ MUST be treated as hits; queries with similarity $< 0.9500$ MUST be treated as misses to prevent semantic drift.
- **Empty or Whitespace-Only Prompts**: Must be rejected during request validation before reaching the embedding engine.
- **Long Prompts Exceeding Tokenizer Context**: Prompts exceeding 512 subword tokens must be deterministically truncated to the maximum supported sequence length with a debug log rather than crashing or throwing unhandled exceptions.
- **Vector Index Cold Start**: When the vector index does not exist at startup, the gateway must automatically create the RediSearch index schema (HNSW/FLAT cosine metric, 384 dimensions) or fail open cleanly if unsupported.
- **Concurrent Indexing (Thundering Herd)**: Multiple concurrent identical or near-identical misses must safely update the vector index without race conditions or index corruption.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST compute dense 384-dimensional float embeddings locally using embedded CPU execution of `all-MiniLM-L6-v2`.
- **FR-002**: System MUST apply L2 vector normalization ($\|V\|_2 = 1.0$) to all generated embeddings prior to similarity calculation or storage.
- **FR-003**: System MUST maintain the total memory footprint of the embedded inference engine below 500MB RAM.
- **FR-004**: System MUST check Tier 1 exact cache first; only upon a Tier 1 miss shall Tier 2 semantic embedding generation and vector search be executed.
- **FR-005**: System MUST perform top-1 nearest neighbor vector search against stored completions using cosine distance.
- **FR-006**: System MUST serve a cached completion from Tier 2 if and only if the vector cosine similarity score is $\ge 0.95$.
- **FR-007**: System MUST set `cache_hit = true` and `cache_tier = "semantic"` on completions returned from Tier 2.
- **FR-008**: System MUST forward requests to the inner completion backend whenever both Tier 1 and Tier 2 report cache misses.
- **FR-009**: System MUST index successful upstream completions into both Tier 1 exact cache and Tier 2 semantic vector index.
- **FR-010**: System MUST support configurable TTL expiration on stored semantic cache records (default 86,400s).
- **FR-011**: System MUST honor `no_cache = true` on incoming requests by bypassing both Tier 1 and Tier 2 read lookups while updating cache entries upon upstream completion.
- **FR-012**: System MUST automatically initialize the RediSearch vector index schema with 384-dimensional cosine distance if not already present.
- **FR-013**: System MUST implement fail-open degradation: if embedding inference fails or the vector index is unreachable, the request must fall through to the upstream provider with zero dropped calls.
- **FR-014**: System MUST provide deterministic tokenization handling subword piece encoding and sequence padding/truncation up to 512 tokens.
- **FR-015**: System MUST preserve correlation IDs and request tracking metadata across semantic cache hits.

---

### Key Entities

- **PromptEmbedding**: A 384-element float vector representing the semantic representation of a normalized user prompt, normalized such that its L2 Euclidean norm equals 1.0.
- **SemanticCacheEntry**: The cached payload stored in the vector store containing the prompt text, model identifier, serialized chat completion response, timestamp, and TTL.
- **VectorIndexSchema**: The index specification in the vector store defining the index name (e.g. `idx:semantic_cache`), key prefix (e.g. `cache:semantic:`), indexing algorithm (HNSW or FLAT), dimension size (384), and distance metric (COSINE).
- **SemanticLookupResult**: Data structure encapsulating lookup outcome: `hit` (boolean), `similarity` (float), `response` (ChatCompletionResponse), and `latency_ms` (double).

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Tier 2 semantic cache retrieval latency achieves p95 $< 20\text{ms}$ under local CPU execution.
- **SC-002**: Paraphrased queries with cosine similarity $\ge 0.95$ consistently achieve a 100% semantic cache hit rate.
- **SC-003**: Queries with cosine similarity $< 0.95$ achieve a 0% false positive cache hit rate (zero semantic pollution).
- **SC-004**: Total gateway memory overhead attributable to local embedding inference remains strictly $< 500\text{MB}$.
- **SC-005**: 100% request success rate during vector store outages (zero dropped requests via fail-open degradation).
- **SC-006**: Test suite achieves $\ge 90\%$ test coverage across new components with 0 errors or leaks under AddressSanitizer and UndefinedBehaviorSanitizer.

---

## Assumptions

- The `all-MiniLM-L6-v2` ONNX model weights and tokenizer vocabulary files are bundled or deterministically downloaded/mocked for offline builds.
- Redis Stack or an instance equipped with the RediSearch module is used for vector search in production and CI; a self-contained in-memory cosine vector index simulation is available for offline unit tests.
- Embeddings are computed over the canonical representation of the prompt (system prompt + user messages) consistent with Tier 1 hashing.
- Cosine distance $D_{\text{cosine}} = 1 - S_{\text{cosine}}$, so similarity $S \ge 0.95$ corresponds to distance $D \le 0.05$.

