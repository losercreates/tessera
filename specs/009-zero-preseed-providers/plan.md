# Implementation Plan: Zero-Preseed Provider Limiter & Telemetry Initialization

**Branch**: `009-zero-preseed-providers` | **Date**: 2026-11-26 | **Spec**: [spec.md](file:///home/mayukh/projects/tessera/specs/009-zero-preseed-providers/spec.md)  
**Input**: Feature specification from `/specs/009-zero-preseed-providers/spec.md`

---

## Summary

Eliminate unconditional hardcoded `groq` and `openrouter` pre-seeding from `DistributedRateLimiter` and `TelemetryRegistry` initialization and reset paths so rate-limit policies and Prometheus provider time-series are registered exclusively from active `ProviderRegistry` configurations (preserving explicit opt-in helpers for standalone unit/concurrency tests). Additionally migrate `tools/gateway_client.cpp` away from legacy hardcoded `FailoverRouter` + outer `RateLimitedBackend(..., "groq")` wiring to match `src/main.cpp`'s cache-first `CachedCompletionBackend -> ModelRouter::CreateFromRegistry(provider_registry, rate_limiter)` architecture while keeping all unit, integration, concurrency, and Prometheus validation gates passing cleanly under ASan and UBSan.

---

## Technical Context

**Language/Version**: C++20 (`-std=gnu++20`, `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`)  
**Primary Dependencies**: STL (atomics, mutexes, unordered/ordered maps, chrono), gRPC++, OpenSSL, mocked/socket Redis client (`MockRedisClient` / `RedisConnectionPool`), GoogleTest (`tests/gtest_compat`), Bash/grep Prometheus validator  
**Storage**: Redis (or `MockRedisClient`) atomic Lua token buckets (`ratelimit:<provider>:<window>`); in-memory thread-safe `TelemetryRegistry` maps  
**Testing**: CTest (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`) compiled under Debug + ASan/UBSan (`-fsanitize=address,undefined`), plus `scripts/verify_metrics_format.sh`  
**Target Platform**: Linux (CMake 3.25+ with Ninja, GCC/Clang)  
**Project Type**: Native C++20 distributed API gateway library (`tessera_core`), server binary (`gateway_server`), and CLI client (`gateway_client`)  
**Performance Goals**: Zero mutex contention regression on `TelemetryRegistry` reads/writes (<1us overhead); zero extraneous Redis Lua calls or phantom provider loops on scrape  
**Constraints**: Strict C++20 RAII (`std::shared_ptr`/`std::unique_ptr`), zero raw owning pointers, full ASan/UBSan cleanliness, backward-compatible opt-in helpers for legacy test fixtures, offline smoke capability for `gateway_client` without live API keys  
**Scale/Scope**: Updates ~12 core header/source/test/CLI files across `ratelimit`, `telemetry`, `routing`, `tools`, and unit/integration tests

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Design Check | Post-Design Check | Notes |
|---|---|---|---|
| **I. Modern C++20 Architecture & Async Concurrency** | PASS | PASS | Strictly uses RAII (`std::shared_ptr`), thread-safe mutexes/atomics; decouples `ProviderRegistry` catalog from vendor-specific pre-seeding and modernizes `gateway_client` layering. |
| **II. Determinism, Reliability & Fault-Tolerant Routing** | PASS | PASS | Standard RPM/RPD policies still enforce deterministically via atomic Redis Lua when opted into or configured; unconfigured providers allow fail-open/unconstrained cleanly. |
| **III. Embedded Local Semantic Inference & Two-Tier Caching** | PASS | PASS | Upgrading `gateway_client` from `RateLimitedBackend -> CachedCompletionBackend -> FailoverRouter` to `CachedCompletionBackend -> ModelRouter` aligns CLI behavior with Feature 008 cache preemption. |
| **IV. Strict Quality Gate & Testing Mandate** | PASS | PASS | Existing tests updated solely to call explicit opt-in helpers (`LoadStandardPolicies()`); new tests verify empty initialization, `Reset()`, custom-only catalog parity, and sanitizer cleanliness across all 4 CTest targets. |
| **V. CI/CD Reproducibility, Tooling & Observability** | PASS | PASS | Compiles cleanly with strict warning flags; `scripts/verify_metrics_format.sh` passes dynamically without hardcoded phantom vendor series. |

---

## Project Structure

### Documentation (this feature)

```text
specs/009-zero-preseed-providers/
├── plan.md                                  # This file
├── research.md                              # Phase 0 design decisions and rationale
├── data-model.md                            # Phase 1 entity state and validation rules
├── quickstart.md                            # Phase 1 end-to-end validation scenarios
├── contracts/
│   └── zero_preseed_contract.md             # Phase 1 C++ interface & CLI behavior contracts
└── tasks.md                                 # Phase 2 output (/speckit-tasks command)
```

### Source Code (repository root)

```text
include/tessera/
├── ratelimit/rate_limiter.hpp               # Default load_standard_policies = false + LoadStandardPolicies() opt-in
├── telemetry/telemetry_registry.hpp         # RegisterStandardProviders() opt-in helper
└── routing/
    ├── provider_registry.hpp                # LoadFromEnvironment(..., ClientFactory = nullptr)
    └── model_router.hpp                     # GetDefaultCascade() const accessor

src/
├── ratelimit/rate_limiter.cpp               # Remove ctor LoadStandardPolicies() call
├── telemetry/telemetry_registry.cpp         # Remove groq/openrouter pre-seeding from ctor and Reset()
└── routing/
    ├── provider_registry.cpp                # Pass optional ClientFactory into LoadFromEnvironment()
    └── model_router.cpp                     # Implement GetDefaultCascade() const

tools/
└── gateway_client.cpp                       # Construct pipeline via ProviderRegistry + ModelRouter + CachedCompletionBackend

tests/
├── unit/
│   ├── test_rate_limiter.cpp                # Explicit opt-in LoadStandardPolicies() + zero-preseed tests
│   ├── test_telemetry.cpp                   # Assert clean zero-preseed ctor/Reset(), non-mutating reads, RegisterStandardProviders()
│   └── test_provider_registry.cpp           # Custom-provider parity test & custom ClientFactory LoadFromEnvironment coverage
└── integration/
    ├── test_ratelimit_concurrency.cpp       # Explicit opt-in LoadStandardPolicies()
    ├── test_redis_resilience.cpp            # Explicit opt-in LoadStandardPolicies()
    └── test_provider_rate_isolation.cpp     # Catalog-only registration zero-phantom-provider assertion (or equivalent unit/int test)
```

**Structure Decision**: Reuse Tessera's existing CMake static-library + executable modular structure (`tessera_core`, `gateway_server`, `gateway_client`, and GTest targets), keeping every header and implementation change strictly scoped to initialization, registration, CLI wiring, and targeted tests.

---

## Complexity Tracking

No Constitution violations identified. All changes simplify initialization paths and remove implicit hardcoded vendor coupling.
