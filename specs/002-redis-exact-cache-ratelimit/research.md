# Phase 0 Research: Redis Exact-Match Cache & Distributed Rate Limiting

**Feature**: `002-redis-exact-cache-ratelimit`  
**Date**: 2026-11-02  
**Status**: Completed  

---

## Technical Decisions & Trade-Offs

### 1. Redis Client & Connection Pool Architecture

* **Decision**: Implement an `IRedisClient` abstraction with a thread-safe `RedisConnectionPool` and an in-memory `MockRedisClient` for hermetic testing.
* **Rationale**:
  - Thread safety: gRPC worker threads concurrently executing requests must acquire connections from a pool without socket contention.
  - Testability: Enables 100% unit test coverage and sanitizer-clean execution without requiring a live external Redis server, while enabling live Redis Stack container verification during integration testing.
  - Fail-Open Resiliency: Connection pool tracks health status and triggers graceful degradation if Redis goes down.
* **Alternatives Considered**:
  - *Raw single hiredis connection*: Thread-unsafe and creates a catastrophic bottleneck.
  - *Hard requirement on local Redis daemon*: Breaks CI and local developer setup without pre-running services.

---

### 2. Atomic Token Bucket via Redis Lua Script

* **Decision**: Deploy a single atomic Lua script (`rate_limiter.lua`) that computes token replenishment dynamically using Redis server time (`redis.call('TIME')`).
* **Rationale**:
  - Atomicity: Eliminates read-modify-write race conditions across multiple distributed gateway replicas.
  - Server-Authoritative Clock: Eliminates reliance on gateway host clocks, preventing clock drift from skewing quota enforcement.
  - Minimal Network Overhead: Evaluates token availability, refills, and returns `{allowed, remaining, retry_after_ms}` in a single network round-trip.
* **Alternatives Considered**:
  - *Client-side rate limit math with Redis INCR/EXPIRE*: Susceptible to race conditions during multi-instance burst periods.
  - *Fixed-window counter*: Suffers from 2x burst boundary problems (e.g., 30 requests at second 59 and 30 requests at second 01).

---

### 3. SHA-256 Canonical Prompt Hashing

* **Decision**: Standardize a canonical serialization string for ChatCompletion requests (`model=<model>|temp=<temp>|msgs=[(role, content), ...]`) and compute SHA-256 hex digest using OpenSSL `EVP_sha256`.
* **Rationale**:
  - Guarantees identical prompts generate identical 64-character hex keys (`cache:exact:<sha256>`).
  - Fast cryptographic hashing (sub-microsecond execution time on modern CPUs).
  - Fixed-length keys fit optimally into Redis hash tables and indices.
* **Alternatives Considered**:
  - *Raw prompt string as Redis key*: Can exceed Redis key length limits or cause memory bloat on large prompts.
  - *MD5 or MurmurHash*: MD5 is cryptographically weak with higher collision probability; 64-bit MurmurHash has non-negligible collision risk at scale.

---

### 4. Cache Tier Architecture: `CachedCompletionBackend`

* **Decision**: Implement the Decorator pattern wrapping `ICompletionBackend` with `CachedCompletionBackend`.
* **Rationale**:
  - Clean separation of concerns: The gRPC server and CallData state machine do not need any Redis-specific logic.
  - Pluggable: The inner backend can be `MockEchoBackend` (Milestone 1/2) or `RouterCompletionBackend` (Milestones 4).
  - Fail-Open Safety: Catches Redis connection errors, logs warnings, and transparently passes the request through to the inner backend.

