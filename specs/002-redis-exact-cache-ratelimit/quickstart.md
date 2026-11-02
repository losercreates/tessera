# Quickstart Guide: Redis Exact-Match Cache & Distributed Rate Limiting

**Feature**: `002-redis-exact-cache-ratelimit`  
**Date**: 2026-11-02  
**Status**: Ready for Verification  

---

## Prerequisites

* **Operating System**: Linux (Ubuntu 22.04+ or Debian 12+)
* **Compiler**: GCC 13+ or Clang 17+
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
Run the exact-match cache benchmark:

```bash
./build/cache_benchmark --requests=1000 --identical=true
```
**Expected Outcome**:
* Request 1: Cache Miss ($\approx 10\text{ms}$ upstream invocation); writes to Redis.
* Requests 2–1000: Cache Hit ($< 2\text{ms}$ p95 latency); returns cached response with `cache_hit: true` and zero backend compute.

---

### Scenario 2: Distributed Rate Limiting (Groq 30 RPM Quota)
Run the rate limiter verification test:

```bash
./build/rate_limiter_test --provider=groq --requests=40 --window-seconds=60
```
**Expected Outcome**:
* Requests 1–30: Permitted (`allowed = true`).
* Requests 31–40: Throttled (`allowed = false`, `Status: RESOURCE_EXHAUSTED`), returning `retry_after_ms`.

---

### Scenario 3: Resilient Fail-Open Cache Degradation
Run the resilience chaos test:

```bash
./build/resilience_test --simulate-redis-outage=true
```
**Expected Outcome**:
* When Redis is unreachable, gateway logs a warning and transparently processes queries through the underlying completion backend with zero dropped client requests.

---

### Scenario 4: Selective Cache Bypass
Submit a query with `--no-cache`:

```bash
./build/gateway_client --message "Generate dynamic forecast" --no-cache
```
**Expected Outcome**:
* Bypasses existing cache lookup, fetches a fresh completion, and updates the cache.

