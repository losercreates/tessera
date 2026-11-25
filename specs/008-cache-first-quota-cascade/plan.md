# Implementation Plan: Cache-First Pipeline Ordering & Quota-Aware Cascade Failover

**Branch**: `008-cache-first-quota-cascade` | **Date**: 2026-11-25 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/008-cache-first-quota-cascade/spec.md`

---

## Summary

Restructure the completion execution pipeline in `src/main.cpp` so `CachedCompletionBackend` sits ahead of routing and rate limiting—preventing Tier 1 exact-match and Tier 2 semantic-match cache hits from consuming provider quota tokens or failing on rate saturation. Move per-provider distributed rate-limit enforcement (`DistributedRateLimiter::CheckAndConsume`) directly into each candidate admission step inside `CascadeRouter::Complete()` (propagated via `ModelRouter::CreateFromRegistry` and optionally `FailoverRouter`) so local token-bucket depletion transparently triggers failover across $N$ priority tiers without tripping outage circuit breakers or making wasted upstream network calls. Add `ICircuitBreaker::CancelProbe()` to avoid latching `HALF_OPEN` probe slots when local quota denies a trial attempt, expose dedicated local rate-limit skip attribution in `CascadeRouterStats` / `TelemetryRegistry`, and distinguish all-local quota exhaustion (`Status::ResourceExhausted`) from upstream exhaustion (`Status::Unavailable`).

---

## Technical Context

**Language/Version**: C++20 (GCC 13, Clang 17)  
**Primary Dependencies**: OpenSSL 3.x (`OpenSSL::SSL`, `OpenSSL::Crypto`), POSIX Sockets, Threads, CMake 3.25+, Ninja, GoogleTest  
**Storage**: Redis Stack (`ExactCacheService` + `DistributedRateLimiter` atomic Lua token buckets) and Semantic Vector Store  
**Testing**: GoogleTest / GoogleMock (with built-in `gtest_compat`), CTest, AddressSanitizer (`ASan`), UndefinedBehaviorSanitizer (`UBSan`)  
**Target Platform**: Linux (Ubuntu 24.04 / Docker containerized runtime)  
**Project Type**: High-performance C++20 distributed API gateway daemon & core static library (`tessera_core`)  
**Performance Goals**: Zero extra Redis rate-limit round-trips on cache hits (< 2ms Tier 1, < 20ms Tier 2), sub-millisecond in-memory routing admission, exact distributed quota compliance under high concurrency  
**Constraints**: Strict RAII ownership (`std::shared_ptr`/`std::unique_ptr`), `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` clean compilation, 0 leaks / UB under ASan/UBSan, full backward compatibility for existing test suites and legacy adapters  
**Scale/Scope**: Core pipeline and routing refactor touching `main.cpp`, `CascadeRouter`, `ModelRouter`, `FailoverRouter`, `CircuitBreaker`, `TelemetryRegistry`, and verification suites

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Requirement | Pre-Design | Post-Design | Justification & Architecture Notes |
| :--- | :--- | :---: | :---: | :--- |
| **I. Modern C++20 Architecture & Async Concurrency** | Strict RAII, zero raw pointer ownership (`std::shared_ptr`/`std::unique_ptr`), modular boundaries | **PASS** | **PASS** | `DistributedRateLimiter` lifetime managed via `std::shared_ptr`; cache layer strictly isolated ahead of routing and quota enforcement. |
| **II. Determinism, Reliability & Fault-Tolerant Routing** | Atomic Redis Lua token buckets, deterministic 3-state circuit breakers (`CLOSED`, `OPEN`, `HALF-OPEN`), zero dropped calls | **PASS** | **PASS** | Open circuits never deduct tokens (`CanAttempt()` checked prior to `CheckAndConsume()`); `CancelProbe()` keeps `HALF-OPEN` deadlock-free; local quota skips failover cleanly without false outage trips. |
| **III. Embedded Local Semantic Inference & Two-Tier Caching** | Two-tier cache evaluated before upstream routing; successful completions from any tier cached | **PASS** | **PASS** | Placing `CachedCompletionBackend` outside `ModelRouter` enforces cache preemption prior to rate limiting and caches 2xx responses recovered via fallback tiers. |
| **IV. Strict Quality Gate & Testing Mandate** | >= 90% coverage, 100% pass rate, zero ASan/UBSan violations, chaos & concurrency tests | **PASS** | **PASS** | Dedicated unit and multi-threaded integration tests verify cache preemption under 0-token quota, 3-tier quota cascade failover, `HALF-OPEN` probe safety, and accurate telemetry under ASan/UBSan. |
| **V. CI/CD Reproducibility & Observability** | CMake/Ninja builds, OpenTelemetry/Prometheus format metrics (`gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`) | **PASS** | **PASS** | Adds `gateway_ratelimit_skips_total` cleanly separated from `upstream_latency_ms` so synthetic 0ms latencies are never injected on local skips. Verified with `verify_metrics_format.sh`. |

---

## Project Structure

### Documentation (this feature)

```text
specs/008-cache-first-quota-cascade/
├── spec.md                                      # Feature specification
├── plan.md                                      # This implementation plan
├── research.md                                  # Phase 0 architectural & race-safety decisions
├── data-model.md                                # Pipeline topology, entities & admission states
├── quickstart.md                                # Runnable ASan/UBSan validation guide
├── contracts/
│   └── pipeline_execution_contract.md           # Ordering, error & telemetry contracts
├── checklists/
│   └── requirements.md                          # Spec quality gate checklist (16/16 passed)
└── tasks.md                                     # Generated later by /speckit-tasks
```

### Source Code Layout

```text
include/tessera/
├── routing/
│   ├── circuit_types.hpp                        # Extend CascadeTarget (+rate_limiter) & FailoverRouterStats (+rate_limit_skips)
│   ├── circuit_breaker.hpp                      # Add virtual CancelProbe() noexcept to ICircuitBreaker & CircuitBreaker
│   ├── cascade_router.hpp                       # Accept optional DistributedRateLimiter; add rate_limit_skips to CascadeRouterStats
│   ├── model_router.hpp                         # Add rate_limiter injection to ModelRouter & CreateFromRegistry()
│   └── failover_router.hpp                      # Accept optional DistributedRateLimiter for legacy adapter parity
└── telemetry/
    └── telemetry_registry.hpp                   # Add IncrementRateLimitSkipsTotal / GetRateLimitSkipsTotal counters

src/
├── main.cpp                                     # Reorder pipeline: CachedCompletionBackend(ModelRouter(providers, limiter))
├── routing/
│   ├── circuit_breaker.cpp                      # Implement CancelProbe() for atomic HALF-OPEN slot release
│   ├── cascade_router.cpp                       # Per-tier breaker -> limiter admission, CancelProbe, stats & error classification
│   ├── model_router.cpp                         # Wire rate_limiter through CreateFromRegistry() & RegisterRoute() cascades
│   └── failover_router.cpp                      # Forward optional limiter & propagate rate_limit_skips / RESOURCE_EXHAUSTED
└── telemetry/
    └── telemetry_registry.cpp                   # Implement & expose gateway_ratelimit_skips_total in ToPrometheusFormat()

tests/
├── unit/
│   ├── test_cache_first_cascade.cpp             # Unit tests: exact/semantic cache preemption under 0 quota & no_cache bypass
│   ├── test_cascade_router.cpp                  # Unit tests: per-tier quota failover, OPEN-skip non-deduction, CancelProbe, RESOURCE_EXHAUSTED
│   ├── test_model_router.cpp                    # Unit tests: quota-aware cascade creation from ProviderRegistry
│   └── test_telemetry.cpp                       # Unit tests: gateway_ratelimit_skips_total & clean upstream latency isolation
└── integration/
    ├── test_multi_provider_cascade.cpp          # Integration tests: cache hits consume 0 quota under exhausted primary; fallback responses cache & preempt
    └── test_provider_rate_isolation.cpp         # Integration tests: 3-tier concurrent burst quota failover (0 dropped requests across quotas)

scripts/
└── verify_metrics_format.sh                     # Prometheus 0.0.4 format check covering gateway_ratelimit_skips_total

CMakeLists.txt                                   # Registers tests/unit/test_cache_first_cascade.cpp under unit_tests
```

**Structure Decision**: Integrated directly into existing `tessera_core` headers, sources, and test binaries in [`CMakeLists.txt`](file:///home/mayukh/projects/tessera/CMakeLists.txt), preserving API backward compatibility for all existing unit tests, benchmarks, `evaluation_harness`, and `gateway_client`.

---

## Complexity Tracking

> **Constitution Check**: All 5 Constitution Principles PASSED cleanly pre-design and post-design. Zero violations or complexity waivers required.
