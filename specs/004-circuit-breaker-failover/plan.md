# Implementation Plan: Circuit Breaker & Multi-Provider Failover

**Branch**: `004-circuit-breaker-failover` | **Date**: 2026-11-04 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/004-circuit-breaker-failover/spec.md`

---

## Summary

Implement a deterministic 3-state Circuit Breaker (`CLOSED`, `OPEN`, `HALF-OPEN`) and resilient `FailoverRouter` coordinating Groq primary (`llama-3.3-70b-versatile`) and OpenRouter fallback (`google/gemma-2-9b-it`) providers. The system provides instantaneous trip to `OPEN` on HTTP 429 quota exhaustion, consecutive 5xx error trip after 3 failures, truncated exponential backoff cooldown with full randomized jitter, single-probe gated `HALF-OPEN` recovery, and zero-loss transparent failover routing with an offline chaos test harness.

---

## Technical Context

**Language/Version**: C++20 (GCC 13.3+, Clang 18.1+ with `-std=c++20`)  
**Primary Dependencies**: Standard Library (`std::atomic`, `std::chrono`, `std::format`, `std::span`, `std::random`), OpenSSL (`libcrypto`, `libssl`), Threads, optional gRPC/Protobuf  
**Storage**: Thread-safe in-memory atomic state; Redis connection pool for rate limits and caching tiers  
**Testing**: GoogleTest / compatibility harness, GMock, CTest under AddressSanitizer and UndefinedBehaviorSanitizer  
**Target Platform**: Linux x86_64 containerized / native server environments  
**Project Type**: High-performance distributed microservice gateway  
**Performance Goals**: Failover routing decision overhead $< 5\text{ms}$; 100% request preservation (0 dropped calls during upstream 429 storms); sub-millisecond state check latency ($< 50\text{ns}$)  
**Constraints**: Zero memory leaks or undefined behavior under ASan/UBSan; strict RAII; deterministic offline testability via chaos mock  
**Scale/Scope**: Groq primary (30 RPM, 1,000 RPD) to OpenRouter fallback (20 RPM, 50 RPD); handles 1,000+ concurrent requests across 200+ threads  

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Requirement | Plan Compliance Status | Notes / Design Alignment |
|---|---|:---:|---|
| **I. Modern C++20 Architecture** | RAII, smart pointers, zero raw pointer ownership, async decoupling | **PASS** | `CircuitBreaker` and `FailoverRouter` use `std::shared_ptr`, `std::atomic`, strict RAII, and modular decoupling |
| **II. Determinism & Reliability** | Deterministic 3-state circuit breaker, instant 429 trip, full jitter backoff, zero-loss failover | **PASS** | `CircuitBreaker` implements `CLOSED`/`OPEN`/`HALF-OPEN`, instant 429 trip, single-probe `HALF-OPEN`, and transparent retry to OpenRouter |
| **III. Two-Tier Caching** | Exact-match & ONNX semantic cache preservation | **PASS** | `FailoverRouter` integrates cleanly as inner backend beneath `CachedCompletionBackend`, ensuring responses from both primary and fallback are cached |
| **IV. Quality Gate & Testing** | $\ge 90\%$ test coverage, ASan/UBSan clean, chaos fault injection | **PASS** | Includes unit test suite for circuit breaker, integration tests for failover and recovery, and offline chaos fault-injection mock |
| **V. CI/CD Reproducibility** | Hermetic CMake/Ninja builds, reproducible CTest matrix | **PASS** | Pure C++20 standard library implementation with zero external network dependencies required during build or test execution |

---

## Project Structure

### Documentation (this feature)

```text
specs/004-circuit-breaker-failover/
├── spec.md                     # Feature specification
├── plan.md                     # Implementation plan
├── research.md                 # Architectural decisions & research findings
├── data-model.md               # Domain models, structs, & state machine
├── contracts/
│   └── circuit_breaker_contract.md # C++ interface contracts & error mappings
├── quickstart.md               # Quickstart verification & run guide
└── checklists/
    └── requirements.md         # Specification quality checklist
```

### Source Code Layout

```text
include/tessera/
├── routing/
│   ├── circuit_types.hpp       # CircuitState, CircuitBreakerConfig, Stats, UpstreamRequest/Response
│   ├── circuit_breaker.hpp     # ICircuitBreaker interface & CircuitBreaker implementation
│   └── failover_router.hpp     # FailoverRouter (ICompletionBackend decorator)
└── backend/
    ├── upstream_client.hpp     # IUpstreamClient interface & HttpUpstreamClient
    └── mock_upstream_client.hpp # Deterministic chaos mock client with fault injection

src/
├── routing/
│   ├── circuit_breaker.cpp     # Thread-safe 3-state state machine with full jitter backoff
│   └── failover_router.cpp     # Routing logic, transparent retry, model remapping, stats
└── backend/
    ├── upstream_client.cpp     # HTTP client implementation with JSON serialization
    └── mock_upstream_client.cpp # Scriptable response queues and fault simulator

tests/
├── unit/
│   └── test_circuit_breaker.cpp # 3-state transitions, 429 instant trip, backoff bounds
└── integration/
    ├── test_failover_integration.cpp # End-to-end transparent failover from Groq to OpenRouter
    └── test_chaos_mock.cpp          # Fault injection concurrency stress test (timeouts, 503, 429)
```

**Structure Decision**: The resilient routing components reside under `include/tessera/routing/` and `src/routing/`, while upstream transport adapters reside under `include/tessera/backend/` and `src/backend/`. This cleanly separates the routing and circuit breaker decision engine from transport protocols per Constitution Principle I.

---

## Complexity Tracking

> **No Constitution violations detected.** The implementation uses standard C++20 primitives without unnecessary dependencies or unmanaged complexity.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| None | N/A | N/A |
