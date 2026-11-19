# Project Roadmap: Distributed LLM API Gateway with Semantic Caching

## Overview
A high-throughput, fault-tolerant C++20 API gateway sitting between client applications and external LLM providers. It routes requests, enforces distributed rate limits, and semantically caches responses to slash latency by up to 90% and ensure 99.99% availability via automated failover.

---

## System Architecture

```mermaid
graph TD
    Client[Internal Microservices] -->|gRPC / REST| Gateway(C++ API Gateway)
    Gateway --> HashCache[(Redis: Exact Match)]
    HashCache -- Miss --> VectorCache[(Redis Stack: Semantic Cache)]
    VectorCache -- Miss --> Router{Routing Engine}
    Router -->|Primary| Groq[Groq API: Llama 3.3 70B]
    Router -->|Failover / Quota 429| OR[OpenRouter API: Gemma-2-9B]
    Groq & OR --> Telemetry[OpenTelemetry Metrics]
```

---

## Milestone Delivery Board

| Milestone | Feature Directory | Focus Area | Deliverables & Gates | Status |
|---|---|---|---|---|
| **M1** | [`specs/001-grpc-gateway-foundation`](specs/001-grpc-gateway-foundation) | API & Async Ingestion | Unified Protobuf contracts, async gRPC CompletionQueue server, CMake 3.25+ build, GTest baseline, CI workflow | ✅ Completed |
| **M2** | [`specs/002-redis-exact-cache-ratelimit`](specs/002-redis-exact-cache-ratelimit) | Exact Cache & Quotas | Redis connection pool, SHA-256 exact match cache (p95 < 2ms), atomic Lua distributed token bucket (Groq/OpenRouter limits) | ✅ Completed |
| **M3** | [`specs/003-onnx-semantic-cache`](specs/003-onnx-semantic-cache) | Local Semantic Inference | Embedded ONNX Runtime C++ (`all-MiniLM-L6-v2`, 384-d), vector normalization, RediSearch HNSW cosine search (threshold > 0.95) | ✅ Completed |
| **M4** | [`specs/004-circuit-breaker-failover`](specs/004-circuit-breaker-failover) | Resilience & Failover | 3-state Circuit Breaker (`CLOSED`, `OPEN`, `HALF-OPEN`), jittered backoff, Groq & OpenRouter clients, chaos mock outage tests | ✅ Completed |
| **M5** | [`specs/005-benchmarks-and-telemetry`](specs/005-benchmarks-and-telemetry) | Evaluation & Production | OpenTelemetry metrics, Docker Compose environment, 1,000 synthetic query evaluation harness (30% paraphrased duplicates) | ✅ Completed |
| **M6** | [`specs/006-ci-matrix-container-release`](specs/006-ci-matrix-container-release) | CI/CD & Automated Release | GitHub Actions dual-compiler matrix (GCC 13 & Clang 17 ASan/UBSan), automated 1,000-query benchmark gating, Prometheus metrics validator, Docker Compose smoke test | ✅ Completed |
| **M7** | [`specs/007-provider-agnostic-routing`](specs/007-provider-agnostic-routing) | Extensible Routing & Cascades | Provider Registry, N-tier CascadeRouter, dynamic ModelRouter, isolated per-provider rate limits & circuit breakers, dynamic Prometheus metrics | ✅ Completed |

---

## Milestone Details

### Milestone 1 (M1): gRPC Gateway Foundation
- **Goals**: Establish the high-throughput asynchronous network runtime and API contracts.
- **Key Artifacts**: `protos/gateway.proto`, async worker thread pool, CMake build configuration, GoogleTest skeleton.
- **Verification Gate**: Concurrent gRPC client benchmark test; clean build with ASan/UBSan under GCC 13 and Clang 17.

### Milestone 2 (M2): Redis Exact Cache & Distributed Rate Limiter
- **Goals**: Implement Tier 1 exact-match caching and multi-replica rate limiting.
- **Key Artifacts**: Redis client wrapper, `rate_limiter.lua`, exact match cache manager with TTL eviction.
- **Verification Gate**: Integration test container validating sub-2ms cache hits and strict rate throttling under multi-threaded contention.

### Milestone 3 (M3): ONNX Semantic Cache & Vector Search
- **Goals**: Local CPU-based semantic similarity search without external embedding APIs.
- **Key Artifacts**: ONNX Runtime C++ model runner (`all-MiniLM-L6-v2`), C++ tokenizer, RediSearch vector index creator/querier.
- **Verification Gate**: Semantic similarity tests verifying paraphrased queries hit cache (< 20ms) with cosine similarity > 0.95.

### Milestone 4 (M4): Circuit Breaker & Multi-Provider Failover
- **Goals**: Resilient routing engine with automated failover and recovery.
- **Key Artifacts**: State machine `CircuitBreaker`, HTTP/2 client adapters for Groq and OpenRouter, jittered backoff.
- **Verification Gate**: Mock upstream chaos test injecting HTTP 429 quota exhaustion and verifying zero-loss failover to OpenRouter.

### Milestone 5 (M5): Telemetry, Benchmarks & Dockerization
- **Goals**: End-to-end evaluation, observability, and reproducible deployment.
- **Key Artifacts**: OpenTelemetry instrumentation (`gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`), Prometheus text exporter, multi-stage `Dockerfile`, `docker-compose.yml`, standalone `evaluation_harness` CLI tool.
- **Verification Gate**: 1,000-query benchmark run confirming 95.6% p95 latency reduction on warm cached queries, 99.0% token cost savings, > 5,400 RPS throughput, and resident memory bounded at ~55 MB RAM (< 500 MB limit) with zero sanitizer violations.

### Milestone 6 (M6): CI Matrix & Automated Container Release Pipeline
- **Goals**: Production CI/CD automation, compiler matrix verification, and release smoke testing.
- **Key Artifacts**: `.github/workflows/ci.yml` dual-compiler matrix (GCC 13 Release vs Clang 17 Debug with ASan/UBSan), automated 1,000-query benchmark gating (`scripts/verify_ci_gates.py`), Prometheus metrics validator (`scripts/verify_metrics_format.sh`), artifact publishing, and automated Docker Compose container smoke testing job.
- **Verification Gate**: 100% passing CI matrix across compilers, zero leaks under AddressSanitizer, automated SLA gate enforcement, and passing in-container health probes.

### Milestone 7 (M7): Provider-Agnostic Routing & Dynamic Model Cascade
- **Goals**: Decouple gateway from hardcoded upstreams into an extensible catalog supporting arbitrary OpenAI-compatible platforms (cloud & local Ollama/vLLM) with $N$-tier cascades.
- **Key Artifacts**: `ProviderRegistry`, `ModelRouter`, `CascadeRouter`, dynamic `RateLimitedBackend` quota resolver, isolated per-provider Redis rate limits and circuit breakers, dynamic Prometheus labels.
- **Verification Gate**: 100% unit and integration test pass rate under ASan/UBSan; chaos 3-tier failover with zero dropped requests; independent per-provider quota and breaker isolation under multi-threaded concurrency.

