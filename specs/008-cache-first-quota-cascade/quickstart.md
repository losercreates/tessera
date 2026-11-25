# Quickstart & Validation Guide: Cache-First Pipeline Ordering & Quota-Aware Cascade Failover

**Feature**: `008-cache-first-quota-cascade`  
**Date**: 2026-11-25  
**Status**: Planned Validation Scenarios

---

## 1. Prerequisites

Build the project with sanitizers enabled to verify correctness and memory/thread safety simultaneously:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## 2. End-to-End Validation Scenarios

### Scenario 1: Zero-Quota Cache Preemption Under Saturated Primary Quota
Verifies that exact-match and semantic-match cache hits bypass upstream rate-limit token deduction and still succeed even when the primary provider's bucket has 0 tokens left.

```bash
./build/unit_tests --gtest_filter="CacheFirstQuotaCascadeTest.*"
./build/integration_tests --gtest_filter="MultiProviderCascadeIntegrationTest.*"
```
**Expected Outcome**:
- Exact-match and semantic-match hits increment `gateway_cache_hits_total` (`tier="exact"` / `tier="semantic"`).
- `DistributedRateLimiter` remaining token count remains completely unchanged across cached replays.
- Cached replays still return `Status::OK()` (`cache_hit = true`) even after remaining tokens are driven to `0`.

---

### Scenario 2: 3-Tier Quota-Triggered Cascade Failover
Verifies that exhausting local quotas sequentially across Tier 1 (`quota = 2`) -> Tier 2 (`quota = 2`) -> Tier 3 (`quota = 5`) succeeds 100% with zero dropped requests, and returns `RESOURCE_EXHAUSTED` with retry timing only when all tiers in a single-provider or saturated route run dry.

```bash
./build/integration_tests --gtest_filter="ProviderRateIsolationIntegrationTest.*"
```
**Expected Outcome**:
- First 2 requests succeed on Tier 1; requests 3–4 fail over seamlessly to Tier 2 (`rate_limit_skips == 2`); request 5 fails over seamlessly to Tier 3 (`total` completions = 5 / 5, 0 dropped).
- Exhausted single-provider route returns `StatusCode::RESOURCE_EXHAUSTED` with `"Rate limit exceeded"` and retry timing.

---

### Scenario 3: Circuit Integrity, Telemetry Attribution & Concurrent Burst Load
Verifies that proactive local throttling skips never trip upstream circuit breakers (`state == CLOSED`), attribute metrics exclusively to the provider executing or skipping, and hold exact distributed quota caps under concurrent multi-threaded load with 0 ASan/UBSan violations.

```bash
./build/unit_tests --gtest_filter="CascadeRouterTest.*:TelemetryTest.*"
ctest --test-dir build --output-on-failure
```
**Expected Outcome**:
- 100% pass rate across all CTest suites (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`).
- Prometheus exposition includes `gateway_ratelimit_skips_total` separated cleanly from `upstream_latency_ms` and `circuit_breaker_state` gauges.
- Zero AddressSanitizer leaks or UndefinedBehaviorSanitizer diagnostics.
