<!--
SYNC IMPACT REPORT
==================
- Version change: Uninitialized Template -> 1.0.0
- Ratification Date: 2026-11-01
- Last Amended Date: 2026-11-01
- Modified Principles:
  - [PRINCIPLE_1_NAME] -> I. Modern C++20 Architecture & Async Concurrency
  - [PRINCIPLE_2_NAME] -> II. Determinism, Reliability & Fault-Tolerant Routing
  - [PRINCIPLE_3_NAME] -> III. Embedded Local Semantic Inference & Two-Tier Caching
  - [PRINCIPLE_4_NAME] -> IV. Strict Quality Gate & Testing Mandate (NON-NEGOTIABLE)
  - [PRINCIPLE_5_NAME] -> V. CI/CD Reproducibility, Tooling & Observability
- Added Sections:
  - Milestone Architecture & Delivery Lifecycle (M1 through M6)
  - Quality Gates & Verification Standards
- Removed Sections:
  - None (Template placeholders instantiated into concrete governance specifications)
- Deferred Items / Follow-up TODOs:
  - None. All bracketed tokens have been resolved.
-->

# Tessera Distributed LLM API Gateway Constitution

## Core Principles

### I. Modern C++20 Architecture & Async Concurrency
The gateway is built on Modern C++20 standard conventions emphasizing safety, expressiveness, and zero-overhead abstractions.
- Memory Safety & Ownership: Strict RAII across all resources. Raw pointer ownership is strictly prohibited; all allocations must use `std::unique_ptr` or `std::shared_ptr`. Contiguous memory views must leverage `std::span` and text formatting must use `std::format`.
- Asynchronous Concurrency: High-throughput ingestion must use non-blocking asynchronous I/O powered by gRPC C++ Async CompletionQueue and a dedicated worker thread pool. Under no circumstances may blocking network calls stall an I/O completion thread.
- Modular Decoupling: Strict boundary separation must be maintained between:
  1. API Ingestion Layer (Protobuf / gRPC async bindings and REST adapters)
  2. Two-Tier Caching Layer (Exact-match SHA-256 and Semantic ONNX/RediSearch)
  3. Distributed Rate Limiting & Token Bucket Engine (Redis Lua scripts)
  4. Resilient Routing & Circuit Breaker Engine
  5. Upstream Provider HTTP/2 Clients (Groq, OpenRouter)
  6. Observability & Telemetry Subsystems

### II. Determinism, Reliability & Fault-Tolerant Routing
The gateway must operate as deterministic, production-grade infrastructure that guarantees 99.99% availability and quota compliance.
- Atomic Distributed Token Bucket: Rate limiting must be enforced across all distributed gateway instances using atomic Redis Lua scripts to eliminate race conditions. Upstream provider limits must be strictly maintained:
  - Primary Provider (Groq / Llama 3.3 70B): 30 requests per minute (RPM), 1,000 requests per day (RPD).
  - Fallback Provider (OpenRouter / Gemma-2-9B): 20 requests per minute (RPM), 50 requests per day (RPD).
- Deterministic Circuit Breaker: Outages and quota limits must trigger a strict 3-state state machine:
  - `CLOSED`: Normal traffic routed to primary provider (Groq).
  - `OPEN`: Instantly tripped upon HTTP 429 (quota exhausted), consecutive 5xx errors, or timeout thresholds. All requests immediately fail over to fallback (OpenRouter).
  - `HALF-OPEN`: After a configured cooldown window, probe requests test primary provider recovery before resetting to `CLOSED`.
- Exponential Backoff & Jitter: Upstream retries must use truncated exponential backoff with full jitter to avoid thundering herd failures against LLM providers.

### III. Embedded Local Semantic Inference & Two-Tier Caching
The gateway must aggressively reduce latency and upstream token expenditure through layered deterministic and semantic caching.
- Embedded ONNX CPU Inference: Vector embeddings must be computed locally without external network dependencies using Microsoft ONNX Runtime C++ CPU execution for the `all-MiniLM-L6-v2` model (384-dimensional dense vectors), keeping memory consumption below 500MB RAM.
- Two-Tier Cache Hierarchy:
  - Tier 1 (Exact Match): SHA-256 hash lookup in Redis against normalized prompt strings (target p95 latency < 2ms).
  - Tier 2 (Semantic Match): Vector similarity search in Redis Stack (RediSearch HNSW / FLAT cosine index) evaluating L2-normalized prompt embeddings. Responses are served from cache when cosine similarity exceeds 0.95 (target p95 latency < 20ms).
- Cache Invalidation & Freshness: Cached items must respect configurable TTL policies and per-request cache-bypass headers. On cache miss, successful upstream completions must be asynchronously indexed into both cache tiers.

### IV. Strict Quality Gate & Testing Mandate (NON-NEGOTIABLE)
Code quality, stability, and memory safety are enforced through non-negotiable verification gates prior to merge.
- Coverage Threshold: Every core module must maintain a minimum 90% unit test coverage verified via GoogleTest (GTest) and GoogleMock (GMock).
- Sanitizer Cleanliness: All tests and binaries must compile cleanly and execute with zero errors or leaks under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan).
- Integration Test Verification: Comprehensive integration tests must run against a containerized Redis Stack instance to validate Lua script atomicity under high concurrency and RediSearch vector retrieval accuracy.
- Chaos & Fault-Injection Testing: A mock LLM server suite must deterministically inject HTTP 429 quota exhaustion, socket timeouts, and malformed JSON payloads to verify circuit breaker transitions and seamless failovers without dropping requests.

### V. CI/CD Reproducibility, Tooling & Observability
System builds, evaluations, and runtime diagnostics must be completely reproducible and transparent.
- Hermetic Toolchain: Builds are defined using CMake 3.25+ with Ninja, supporting both GCC 13 and Clang 17 under Linux environments, containerized with Docker Compose.
- Automated CI Pipeline: Every pull request and push to the primary branch must pass the GitHub Actions CI workflow, including multi-compiler matrix compilation, sanitizer runs, integration tests, and synthetic benchmark validation.
- Observability Standards: Distributed tracing and real-time metrics must be instrumented using the OpenTelemetry C++ SDK, capturing `gateway_requests_total`, `gateway_cache_hits_total{tier="exact|semantic"}`, `circuit_breaker_state`, and `upstream_latency_ms`.

## Milestone Architecture & Delivery Lifecycle

The gateway project is structured into six sequential, verifiable milestones:

- **Milestone 1 (M1) - Protobuf Contracts & Async gRPC Ingestion**:
  Define unified `ChatCompletion` protobuf contracts (`protos/gateway.proto`), configure CMake 3.25+ build system, and implement asynchronous gRPC multi-threaded server scaffolding.
- **Milestone 2 (M2) - Exact-Match Cache & Atomic Lua Rate Limiting**:
  Implement Redis connection pooling, SHA-256 exact-match caching tier, and Redis Lua scripts for distributed token bucket enforcement for Groq and OpenRouter quotas.
- **Milestone 3 (M3) - ONNX Semantic Inference & Vector Search**:
  Integrate ONNX Runtime C++ with `all-MiniLM-L6-v2` model, implement tokenization and vector normalization, and establish RediSearch HNSW cosine vector index for semantic caching.
- **Milestone 4 (M4) - Upstream Adapters, Circuit Breakers & Failover Engine**:
  Implement HTTP/2 client adapters for Groq and OpenRouter, 3-state circuit breaker state machine, jittered exponential backoff, and automated failover routing.
- **Milestone 5 (M5) - Extensive Testing Suite & Chaos Fault Injection**:
  Deliver >= 90% unit test coverage (GoogleTest), Redis Stack integration suite, mock LLM server chaos tests, and sanitizer-clean execution.
- **Milestone 6 (M6) - Evaluation Harness, Docker Compose & GitHub Actions CI**:
  Build synthetic evaluation harness (1,000 queries with 30% paraphrased duplicates), benchmark p95 latency and RPS under load, configure Docker Compose, and establish full GitHub Actions CI matrix.

## Quality Gates & Verification Standards

To merge code or declare milestone completion, all artifacts must satisfy:
1. **Compilation**: Clean build under both GCC 13 and Clang 17 with `-Wall -Wextra -Werror -pedantic`.
2. **Sanitizers**: Zero warnings or errors under ASan, UBSan, and TSan (ThreadSanitizer).
3. **Unit Tests**: 100% pass rate across GoogleTest suite with >= 90% line coverage.
4. **Integration**: Successful end-to-end execution against Redis Stack and mock upstream servers.
5. **Linting & Formatting**: Strict adherence to `.clang-format` and `.clang-tidy`.

## Governance

This Constitution acts as the binding technical authority for the Tessera Distributed LLM API Gateway.
- Precedence: The principles and standards specified herein supersede all ad-hoc conventions or contradictory task descriptions.
- Amendments: Any modifications to core principles, quota budgets, or testing mandates require a formal pull request documenting architectural justification, impact analysis, and a semantic version bump.
- Versioning Rules:
  - `MAJOR`: Fundamental architectural paradigm shifts or removal of core principles.
  - `MINOR`: Addition of new architectural principles, milestone definitions, or expanded quality gates.
  - `PATCH`: Editorial refinements, documentation clarifications, and non-semantic corrections.
- Compliance: Automated CI workflows and code reviews must enforce compliance with this document at every milestone.

**Version**: 1.0.0 | **Ratified**: 2026-11-01 | **Last Amended**: 2026-11-01
