# Feature Specification: Redis Exact-Match Cache & Distributed Rate Limiting

**Feature Branch**: `002-redis-exact-cache-ratelimit`

**Created**: 2026-11-02

**Status**: Draft

**Input**: User description: "Build Milestone 2: Redis Exact-Match Cache & Distributed Rate Limiting. Implement Redis connection pooling, Tier 1 SHA-256 exact-match caching with configurable TTL, and atomic distributed token-bucket rate limiting via Redis Lua scripts enforcing Groq (30 RPM / 1000 RPD) and OpenRouter (20 RPM / 50 RPD) quotas across gateway replicas."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Exact-Match Caching for Identical Queries (Priority: P1)

As an internal microservice submitting repetitive LLM queries, I want identical prompts to return cached responses within 2 milliseconds, so that my application experiences ultra-low latency and avoids redundant upstream token consumption.

**Why this priority**: Repeated queries (such as boilerplate classification, recurring system prompts, or identical search queries) represent up to 40% of enterprise LLM workloads. Immediate exact-match retrieval directly reduces costs and latency.

**Independent Test**: Can be fully tested by submitting a prompt, verifying it returns a fresh response and populates the cache, then submitting the exact same prompt and verifying that the response is returned from Tier 1 cache in under 2ms with matching payload and token usage metadata.

**Acceptance Scenarios**:

1. **Given** no prior cache entry for a prompt, **When** a client submits a ChatCompletion request, **Then** the gateway executes the backend, returns the response, and writes the response into Redis under a SHA-256 key with the configured TTL.
2. **Given** an existing cache entry for a prompt, **When** a client submits an identical request, **Then** the gateway retrieves the entry from Redis and returns it in $< 2\text{ms}$ with `cache_hit: true` and `tier: "exact"`, without invoking downstream backends.

---

### User Story 2 - Distributed Token Bucket Rate Limiting (Priority: P1)

As a platform engineering team running multiple gateway replicas, I want rate limits enforced atomically across all replicas according to provider quotas, so that sudden traffic spikes do not exceed provider limits or trigger upstream bans.

**Why this priority**: Free and enterprise LLM providers enforce hard RPM (Requests Per Minute) and RPD (Requests Per Day) ceilings. Breaching them leads to catastrophic HTTP 429 cascades across all microservices.

**Independent Test**: Can be fully tested by running multiple concurrent worker threads simulating requests across instances against Groq (30 RPM / 1,000 RPD) and OpenRouter (20 RPM / 50 RPD) quotas, verifying that exactly the allowed quota passes and all excess requests receive structured `RESOURCE_EXHAUSTED` errors with retry-after timestamps.

**Acceptance Scenarios**:

1. **Given** a quota budget of 30 RPM for Groq, **When** clients send 35 requests within a 1-minute window, **Then** the first 30 requests are permitted and the remaining 5 are rejected with status `RESOURCE_EXHAUSTED` and the seconds remaining until quota replenishment.
2. **Given** multiple gateway replicas communicating with Redis, **When** concurrent requests arrive simultaneously, **Then** Redis Lua scripts atomically deduct tokens without race conditions or double-spending.

---

### User Story 3 - Redis Connection Resiliency & Graceful Degradation (Priority: P2)

As an operations engineer, I want the gateway to continue serving requests even if the Redis cache instance becomes temporarily unreachable, so that cache outages do not cause service outages.

**Why this priority**: Caching is a performance optimization, not a point of total system failure. The gateway must fail open for cache reads/writes rather than rejecting customer traffic.

**Independent Test**: Can be fully tested by stopping or partitioning the Redis instance during active traffic and verifying that requests continue to execute through the underlying completion backend with warning logs, then reconnecting Redis and verifying caching resumes automatically.

**Acceptance Scenarios**:

1. **Given** Redis is unreachable, **When** a client submits a ChatCompletion request, **Then** the gateway logs a warning, bypasses the cache, processes the request via the completion backend, and returns a successful response without failing the client call.
2. **Given** Redis connection is restored after a network partition, **When** new requests arrive, **Then** the gateway connection pool automatically reconnects and resumes caching.

---

### User Story 4 - Cache Expiry & Selective Bypass (Priority: P2)

As a client developer testing new prompt versions, I want the ability to bypass the cache on demand and configure cache TTLs, so that stale completions are not returned during testing or volatile prompt evaluations.

**Why this priority**: Prevents stale responses when prompt templates are being tweaked or when data volatility requires real-time model completions.

**Independent Test**: Can be fully tested by sending a request with `no_cache = true` for an already cached query and verifying that a fresh completion is generated and the cache entry is updated.

**Acceptance Scenarios**:

1. **Given** a prompt already cached, **When** a request includes a `no_cache: true` directive, **Then** the gateway bypasses cache lookup, queries the backend, and overwrites the cached value with the new response.
2. **Given** a cached response with a 10-second TTL, **When** queried after 11 seconds, **Then** the key is expired and a fresh completion is fetched.

---

### Edge Cases

- **Redis Server Clock Drift**: Gateway replicas may have minor clock discrepancies. The Lua token bucket script uses Redis server-side time (`redis.call('TIME')`) as the authoritative clock to prevent token calculation skew.
- **Thundering Herd / Cache Stampede**: Multiple concurrent requests for the same uncached prompt do not corrupt cache state; the fastest completion writes to Redis and subsequent responses refresh the TTL.
- **Large Response Payloads**: Responses exceeding 512KB are serialized efficiently into compressed JSON binary strings before storage.
- **Zero Token Availability**: When a client requests quota when remaining tokens are 0, the script returns remaining tokens = 0 and exact milliseconds until the next token replenishes.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST compute a deterministic SHA-256 hash over normalized prompt parameters (model, messages content/role, temperature) to construct Tier 1 exact-match cache keys (`cache:exact:<sha256>`).
- **FR-002**: The system MUST query Redis for exact-match cache entries and return matching cached `ChatCompletionResponse` payloads with `cache_hit = true` and `tier = "exact"`.
- **FR-003**: The system MUST asynchronously serialize and store successful completion responses in Redis with a configurable TTL (default: 86,400 seconds / 24 hours).
- **FR-004**: The system MUST support an optional per-request `no_cache` flag that bypasses cache reading and forces fresh generation.
- **FR-005**: The system MUST execute token bucket rate limiting using an atomic server-side Redis Lua script that tracks token balance, last refill timestamp, and capacity.
- **FR-006**: The system MUST enforce two independent rate limit buckets per provider:
  - **Groq Primary**: 30 requests per minute (RPM) and 1,000 requests per day (RPD).
  - **OpenRouter Fallback**: 20 requests per minute (RPM) and 50 requests per day (RPD).
- **FR-007**: When quota is exceeded, the system MUST immediately return a `RESOURCE_EXHAUSTED` status including metadata for `retry_after_seconds` and remaining quota count.
- **FR-008**: The system MUST manage a thread-safe Redis connection pool supporting configurable pool size, connection timeouts, and automatic reconnection backoff.
- **FR-009**: The system MUST implement a fail-open policy for cache lookups: if Redis times out or fails, the gateway MUST bypass the cache and execute downstream backends without dropping the request.

### Key Entities *(include if feature involves data)*

- **ExactCacheKey**: Deterministic SHA-256 hash string derived from canonical representation of `(model, messages, temperature)`.
- **CachedCompletion**: Cached binary payload stored in Redis containing serialized `ChatCompletionResponse`, original creation timestamp, and upstream model identifier.
- **TokenBucketState**: Redis hash storing rate limit state for a provider:
  - `tokens`: Current available float tokens in bucket.
  - `last_refill_ms`: Timestamp in milliseconds of last replenishment.
  - `capacity`: Maximum burst token capacity.
  - `fill_rate_per_sec`: Token replenishment rate per second.
- **RateLimitResult**: Evaluation result returned by the Lua script:
  - `allowed`: Boolean indicating whether request is admitted.
  - `remaining_tokens`: Number of tokens remaining after consumption.
  - `retry_after_ms`: Milliseconds until enough tokens are replenished if rejected.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Exact-match cache hit latency overhead is under 2 milliseconds for 95% of cached queries.
- **SC-002**: Distributed rate limiter allows exactly the configured quota (e.g. 30 requests in a 60-second window for Groq) with 0% overage under 200 concurrent threads across simulated replicas.
- **SC-003**: 100% of rate-limited requests receive actionable `RESOURCE_EXHAUSTED` responses with accurate retry-after seconds within 1 millisecond.
- **SC-004**: Zero dropped requests during simulated temporary Redis disconnects (fails open for caching with graceful degradation).
- **SC-005**: Automated unit and integration test suites achieve greater than 90% statement coverage across caching and rate-limiting modules.
- **SC-006**: The gateway codebase compiles cleanly with zero leaks or errors under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan).

## Assumptions

- Redis 7+ or Redis Stack is available locally (port 6379) or via container for integration testing.
- When Redis is unavailable, rate limiting defaults to an in-memory local fallback or permissive pass-through mode to avoid total gateway outage, logging high-priority alerts.
- SHA-256 hashing is computed using standard OpenSSL `EVP_sha256` or a self-contained C++ SHA-256 implementation.
- Default exact-match TTL is 24 hours (86,400 seconds), configurable via server settings.
