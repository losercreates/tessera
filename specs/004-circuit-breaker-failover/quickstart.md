# Quickstart & Verification Guide: Circuit Breaker & Multi-Provider Failover

**Feature Branch**: `004-circuit-breaker-failover`  
**Date**: 2026-11-04  
**Status**: Completed  

---

## 1. Prerequisites & Build Configuration

Verify the C++20 build environment with sanitizers enabled:

```bash
# Configure build with Ninja and ASan/UBSan
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

# Compile core library and test targets
cmake --build build --parallel $(nproc)
```

---

## 2. Verification Scenarios

### Scenario 1: Deterministic 3-State Circuit Breaker Unit Tests
Validates immediate trip on HTTP 429, 3 consecutive 5xx transitions, and full jitter backoff calculations.

```bash
# Execute unit test suite for circuit breaker
./build/unit_tests --gtest_filter="CircuitBreakerTest.*"
```

**Expected Outcome**:
- `CircuitBreakerTest.ImmediateTripOn429`: State transitions from `CLOSED` to `OPEN` after exactly 1 HTTP 429.
- `CircuitBreakerTest.TripsOnConsecutive5xx`: Transitions to `OPEN` on the 3rd consecutive 500 error; stays `CLOSED` if a 200 resets the streak.
- `CircuitBreakerTest.JitteredBackoffWithinBounds`: All calculated backoffs lie strictly between 0 and truncated maximum.
- All tests pass with zero memory leaks under AddressSanitizer.

---

### Scenario 2: Automated Zero-Loss Failover Integration Test
Simulates an upstream HTTP 429 quota exhaustion on Groq primary during active client completions and verifies transparent failover to OpenRouter fallback with 0 dropped calls.

```bash
# Execute failover integration test
./build/integration_tests --gtest_filter="FailoverIntegrationTest.*"
```

**Expected Outcome**:
- Initial request routed to Groq (`llama-3.3-70b-versatile`) succeeds.
- Groq returns HTTP 429 on the second request.
- Gateway intercepts 429, trips Groq circuit breaker to `OPEN`, retries transparently against OpenRouter (`google/gemma-2-9b-it`), and returns HTTP 200 completion to the caller.
- Subsequent requests route immediately to OpenRouter without calling Groq.
- Aggregate success rate: 100% (0 dropped calls).

---

### Scenario 3: Single-Probe HALF-OPEN Recovery & Concurrent Fallback
Verifies that when Groq's cooldown expires and state transitions to `HALF-OPEN`, exactly one trial request is permitted to probe Groq, while all concurrent requests continue routing to OpenRouter.

```bash
# Execute recovery probe test
./build/integration_tests --gtest_filter="FailoverRecoveryTest.*"
```

**Expected Outcome**:
- Exactly 1 probe thread calls Groq.
- Concurrent client requests during probe execution are served by OpenRouter.
- Upon probe HTTP 200 OK, Groq circuit breaker resets to `CLOSED`.
- Subsequent requests automatically resume routing to Groq primary.

---

### Scenario 4: Chaos Mock Fault Injection & Concurrency Stress
Injects intermittent 429s, socket timeouts, and 503 errors under 100 concurrent threads.

```bash
# Execute chaos fault injection test
./build/integration_tests --gtest_filter="ChaosFaultInjectionTest.*"
```

**Expected Outcome**:
- Total requests processed: $\ge 1,000$.
- Failover decisions complete with $< 5\text{ms}$ latency overhead.
- Clean execution with zero race conditions or sanitizer errors under ThreadSanitizer/ASan.

---

### Scenario 5: Full CTest Matrix Run
Runs all unit and integration test binaries in parallel.

```bash
ctest --test-dir build --output-on-failure --parallel $(nproc)
```

**Expected Outcome**:
- 100% test targets pass (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`).

