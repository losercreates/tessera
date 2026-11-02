# Implementation Plan: Redis Exact-Match Cache & Distributed Rate Limiting

**Branch**: `002-redis-exact-cache-ratelimit` | **Date**: 2026-11-02 | **Spec**: [specs/002-redis-exact-cache-ratelimit/spec.md](spec.md)

**Input**: Feature specification from `specs/002-redis-exact-cache-ratelimit/spec.md`

---

## Summary

Milestone 2 delivers high-performance caching and quota enforcement to the Tessera API Gateway. It introduces a thread-safe `RedisConnectionPool` and client abstraction (`IRedisClient`), Tier 1 SHA-256 exact-match caching (`ExactCacheService`) with sub-2ms retrieval latency and fail-open resilience, and a distributed token-bucket rate limiter (`DistributedRateLimiter`) powered by an atomic server-side Redis Lua script. The rate limiter enforces strict provider quotas across multiple gateway replicas: Primary Groq (30 RPM / 1,000 RPD) and Fallback OpenRouter (20 RPM / 50 RPD), returning structured `RESOURCE_EXHAUSTED` responses when quota is breached.

---

## Technical Context

**Language/Version**: Modern C++20 (`gcc-13`, `clang++-18`)  
**Primary Dependencies**: OpenSSL (`libssl-dev` for SHA-256 cryptographic hashing), POSIX Sockets / hiredis for Redis RESP protocol, C++20 standard library  
**Storage**: Redis 7+ / Redis Stack (local port 6379, containerized for CI)  
**Testing**: GoogleTest (GTest), GoogleMock (GMock), CTest  
**Target Platform**: Linux x86_64 / ARM64  
**Project Type**: Distributed API Gateway Caching & Rate Limiting Subsystem  
**Performance Goals**: Tier 1 cache hit p95 latency $< 2\text{ms}$; rate limit evaluation $< 1\text{ms}$; 0% overage under 200 concurrent threads  
**Constraints**: Zero memory leaks or undefined behavior under ASan/UBSan; fail-open cache degradation on Redis disconnect; server-authoritative time evaluation via Redis `TIME`  
**Scale/Scope**: Sits directly between the async gRPC ingestion layer (M1) and upstream provider routing (M4)  

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate Status | Compliance Details |
|---|---|---|
| **Principle I: C++20 & Async Concurrency** | **PASS** | RAII ownership, smart pointers (`std::unique_ptr`, `std::shared_ptr`), thread-safe connection pool, non-blocking cache lookups. |
| **Principle II: Determinism & Reliability** | **PASS** | Atomic Lua script eliminates distributed race conditions; strict provider quota budgets (Groq: 30 RPM / 1000 RPD; OpenRouter: 20 RPM / 50 RPD). |
| **Principle III: Local Semantic Caching** | **PASS** | Implements Tier 1 Exact Match (SHA-256 prompt hash) with sub-2ms target latency and configurable TTL; prepares interface for Tier 2 Semantic Cache (M3). |
| **Principle IV: Testing & Quality Gate** | **PASS** | $\ge 90\%$ test coverage with GoogleTest; live Redis Stack integration tests; verified cleanly under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan). |
| **Principle V: CI/CD & Observability** | **PASS** | Automated GitHub Actions CI workflow with Redis service container, multi-compiler matrix, and rate-limit metadata export. |

---

## Project Structure

### Documentation (this feature)

```text
specs/002-redis-exact-cache-ratelimit/
├── spec.md              # Feature specification
├── plan.md              # This implementation plan
├── research.md          # Concurrency, hashing & Lua trade-offs
├── data-model.md        # Redis key patterns, schemas & domain entities
├── quickstart.md        # Build and execution verification guide
├── contracts/
│   └── rate_limiter.lua # Atomic Redis Lua script
└── checklists/
    └── requirements.md  # Spec quality validation checklist
```

### Source Code Layout

```text
include/tessera/
├── cache/
│   ├── sha256.hpp             # Deterministic SHA-256 prompt hashing
│   ├── redis_client.hpp       # IRedisClient interface & MockRedisClient
│   ├── redis_pool.hpp         # Thread-safe Redis connection pool
│   └── exact_cache.hpp        # Tier 1 ExactCacheService & CachedCompletionBackend
└── ratelimit/
    ├── token_bucket.hpp       # RateLimitRule & TokenBucket math
    └── rate_limiter.hpp       # DistributedRateLimiter (Redis Lua executor)

src/
├── cache/
│   ├── sha256.cpp
│   ├── redis_client.cpp
│   ├── redis_pool.cpp
│   └── exact_cache.cpp
└── ratelimit/
    ├── token_bucket.cpp
    └── rate_limiter.cpp

tests/
├── unit/
│   ├── test_sha256.cpp              # Hashing & canonical prompt string tests
│   ├── test_exact_cache.cpp         # Exact cache hit/miss/bypass logic tests
│   └── test_rate_limiter.cpp        # Token bucket calculation & quota tests
└── integration/
    ├── test_redis_integration.cpp   # Live Redis exact cache integration tests
    └── test_ratelimit_concurrency.cpp # 200-thread concurrent rate limit test
```

**Structure Decision**: Placed modular components in `include/tessera/cache/` and `include/tessera/ratelimit/`, integrating cleanly as decorators around `ICompletionBackend` without modifying the core gRPC server.

---

## Complexity Tracking

> *No constitutional violations detected. Architecture strictly adheres to all non-negotiables.*

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| *None* | N/A | N/A |
