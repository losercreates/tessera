# Quickstart Guide: Redis Exact-Match Cache & Distributed Rate Limiting

**Feature**: `002-redis-exact-cache-ratelimit`  
**Date**: 2026-11-02  
**Status**: Ready for Verification  

---

## Prerequisites

* **Operating System**: Linux (Ubuntu 22.04+ or Debian 12+)
* **Compiler**: GCC 13+ or Clang 17+ (C++20 compliant)
* **Dependencies**: OpenSSL development headers (`libssl-dev`), Redis / Redis Stack (local port 6379 or container)

```bash
# Install OpenSSL development headers if not present
sudo apt-get install -y libssl-dev
```

---

## Build Instructions

```bash
# Build release binaries with tests enabled
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

With AddressSanitizer and UndefinedBehaviorSanitizer:
```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## Verification Scenarios

### Scenario 1: Exact-Match Cache Retrieval (< 2ms p95)
Run the exact-match cache benchmark measuring 10,000 queries:

```bash
./build/cache_benchmark --iterations=10000
```
**Expected Outcome**:
* Request 1: Cache Miss; writes to Redis.
* Requests 2–10,000: Cache Hit ($< 2\text{ms}$ p95 latency); returns cached response with `cache_hit: true` and zero backend compute.

---

### Scenario 2: Distributed Rate Limiting (Groq 30 RPM Quota & Concurrency)
Run the 200-thread atomic token-bucket rate limiter test:

```bash
./build/ratelimit_concurrency_test
```
**Expected Outcome**:
* Exactly 30 requests permitted (`allowed = true`).
* 170 concurrent requests throttled (`allowed = false`, `Status: RESOURCE_EXHAUSTED`), returning `retry_after_ms`.
* Zero race condition overages.

---

### Scenario 3: Resilient Fail-Open Cache Degradation
Run integration test suite covering Redis network partition and disconnection:

```bash
ctest --test-dir build -R integration_tests --output-on-failure
```
**Expected Outcome**:
* When Redis is unreachable or partitioned, the gateway logs a warning and transparently processes queries through the underlying completion backend with zero dropped client requests.

---

### Scenario 4: Selective Cache Bypass
Submit a query with `--no-cache`:

```bash
./build/gateway_client --message "Generate dynamic forecast" --no-cache
```
**Expected Outcome**:
* Bypasses existing cache lookup, fetches a fresh completion, and updates the cache (`cache_hit: false`).
