# Implementation Plan: Provider-Agnostic Routing & Dynamic Model Cascade

**Branch**: `007-provider-agnostic-routing` | **Date**: 2026-11-19 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/007-provider-agnostic-routing/spec.md`

---

## Summary

Refactor the gateway routing engine from rigid binary Groq/OpenRouter coupling into an extensible `ProviderRegistry`, generalized $N$-tier `CascadeRouter`, and model-based `ModelRouter`. Upstream platforms are dynamically loaded from declarative configuration (JSON or environment variables), supporting any OpenAI-compatible provider (including local self-hosted runtimes like Ollama/vLLM without API keys, and cloud providers like DeepSeek, OpenAI, Together, and Mistral). Each provider maintains an independent 3-state circuit breaker, dedicated rate limit quota, and dynamically labeled telemetry streams while ensuring zero dropped requests during failovers and remaining 100% clean under AddressSanitizer and UndefinedBehaviorSanitizer.

---

## Technical Context

**Language/Version**: C++20 (GCC 13, Clang 17)  
**Primary Dependencies**: OpenSSL 3.x (`OpenSSL::SSL`, `OpenSSL::Crypto`), POSIX Sockets, Threads, CMake 3.25+, Ninja, GoogleTest  
**Storage**: Redis Stack (Redis 7.x, RediSearch, RedisJSON)  
**Testing**: GoogleTest baseline, GoogleMock, CTest, ASan/UBSan  
**Target Platform**: Linux (Ubuntu 24.04 / containerized Docker environment)  
**Project Type**: High-performance distributed AI gateway service and core library  
**Performance Goals**: Sub-millisecond routing resolution overhead (< 1.0ms p95), > 5,000 RPS gateway throughput  
**Constraints**: < 500 MB peak resident memory RSS, zero memory leaks / undefined behavior under sanitizers, zero dropped calls on failover  
**Scale/Scope**: Supports arbitrary $N$-tier fallback cascades, dynamic model alias mapping, and multi-tenant quota isolation  

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Requirement | Compliance Status | Justification & Architecture Notes |
| :--- | :--- | :---: | :--- |
| **Principle I: Modern C++20 Architecture** | Strict RAII, zero raw pointer ownership, `std::unique_ptr`/`std::shared_ptr`, `std::span` | **PASS** | `ProviderRegistry`, `CascadeRouter`, and `ModelRouter` manage client and breaker lifecycles strictly via `std::shared_ptr`. |
| **Principle II: Determinism & Fault-Tolerant Routing** | Atomic token-bucket rate limits, 3-state circuit breakers (`CLOSED`, `OPEN`, `HALF-OPEN`), full jitter backoff | **PASS** | Every registered provider gets an isolated `CircuitBreaker` and distinct Redis Lua token-bucket rules. |
| **Principle III: Embedded Inference & Two-Tier Caching** | Two-tier cache evaluated before upstream routing, successful completions indexed | **PASS** | Pipeline evaluates Tier 1 exact match and Tier 2 semantic match before invoking `ModelRouter` or `CascadeRouter`. |
| **Principle IV: Strict Quality Gate & Testing Mandate** | >= 90% test coverage, 100% pass rate, zero ASan/UBSan leaks, chaos fault injection | **PASS** | Comprehensive unit and integration test suites testing multi-provider cascades under chaos 429/5xx faults. |
| **Principle V: CI/CD Reproducibility & Observability** | Hermetic CMake/Ninja builds, Prometheus 0.0.4 metrics tagged with provider labels | **PASS** | Metrics dynamically tagged by provider name in `TelemetryRegistry` with zero hardcoding. |

---

## Project Structure

### Documentation (this feature)

```text
specs/007-provider-agnostic-routing/
├── spec.md              # Feature specification
├── plan.md              # This implementation plan
├── research.md          # Architectural decisions & research findings
├── data-model.md        # Entities, relationships, and state machine diagrams
├── quickstart.md        # Validation scenarios and testing instructions
├── contracts/
│   ├── provider_config_contract.md  # Provider configuration JSON & env schema
│   └── model_routing_contract.md    # Model-to-cascade routing specification
└── checklists/
    └── requirements.md  # Quality validation checklist
```

### Source Code Layout

```text
include/tessera/
├── backend/
│   ├── completion_backend.hpp
│   ├── upstream_client.hpp         # Generic OpenAI-compatible HTTP client
│   └── mock_upstream_client.hpp
├── routing/
│   ├── circuit_breaker.hpp         # 3-state Circuit Breaker
│   ├── circuit_types.hpp           # Provider & routing data types
│   ├── cascade_router.hpp          # N-tier cascading fallback router
│   ├── model_router.hpp            # Model-to-provider routing engine
│   ├── provider_registry.hpp       # Catalog of active providers and routes
│   └── failover_router.hpp         # Backward-compatible 2-tier adapter
├── ratelimit/
│   ├── rate_limiter.hpp            # Multi-provider distributed rate limiter
│   └── rate_limited_backend.hpp    # Dynamic per-request rate limiting wrapper
└── telemetry/
    └── telemetry_registry.hpp      # Dynamic multi-provider metrics registry

src/
├── backend/
│   └── upstream_client.cpp         # OpenSSL TLS client (supports local & cloud)
├── routing/
│   ├── cascade_router.cpp          # N-tier execution with zero-loss recovery
│   ├── model_router.cpp            # Model resolution & alias routing
│   ├── provider_registry.cpp       # Config loader & provider catalog
│   └── failover_router.cpp         # Refactored adapter over CascadeRouter
├── ratelimit/
│   └── rate_limited_backend.cpp    # Dynamic provider quota resolution
├── telemetry/
│   └── telemetry_registry.cpp      # Dynamic provider metric instantiation
└── main.cpp                        # Config loader wiring & server startup

tests/
├── unit/
│   ├── test_provider_registry.cpp  # Registry, validation, and config loading
│   ├── test_cascade_router.cpp     # N-tier fallback and breaker tripping
│   └── test_model_router.cpp       # Exact, wildcard, and default routing
└── integration/
    ├── test_multi_provider_cascade.cpp # Chaos failover across 3+ providers
    └── test_provider_rate_isolation.cpp # Quota isolation across providers
```

**Structure Decision**: Fully integrated within existing `tessera_core` library and `tests/` directories, preserving complete backward compatibility for existing binary entrypoints (`gateway_server`, `gateway_client`, `evaluation_harness`).

---

## Complexity Tracking

> **Constitution Check**: All gates PASSED cleanly with zero violations. No complexity exemptions required.
