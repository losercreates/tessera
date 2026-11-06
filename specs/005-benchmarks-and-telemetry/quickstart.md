# Quickstart & Verification Guide: Evaluation Harness, Metrics & Containerized Benchmarks

**Feature Branch**: `005-benchmarks-and-telemetry`  
**Date**: 2026-11-06  
**Status**: Completed  

---

## 1. Prerequisites & Build Configuration

Verify the C++20 build environment with sanitizers and tests enabled:

```bash
# Configure build with Ninja and ASan/UBSan
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

# Compile core library, server, tests, and evaluation tools
cmake --build build --parallel $(nproc)
```

---

## 2. Verification Scenarios

### Scenario 1: OpenTelemetry Metrics Unit Tests
Validates thread-safe metric counter increments, histogram bucket distributions, circuit breaker gauge reflection, and Prometheus text formatting.

```bash
# Execute telemetry unit test suite
./build/unit_tests --gtest_filter="TelemetryTest.*"
```

**Expected Outcome**:
- `TelemetryTest.RequestsTotalIncrementsPerLabel`: Accurately counts requests across models and status codes.
- `TelemetryTest.CacheHitsTierPartitioning`: Distinctly tracks exact, semantic, and miss counters.
- `TelemetryTest.PrometheusTextFormatSerialization`: Outputs well-formed `# HELP` and `# TYPE` blocks.
- Execution completes with 0 errors or leaks under AddressSanitizer.

---

### Scenario 2: 1,000-Query Synthetic Evaluation Harness
Executes the full 1,000-query workload containing 30% paraphrased duplicates against the Two-Tier cache pipeline to verify latency reduction and token savings.

```bash
# Run evaluation harness
./build/evaluation_harness --queries=1000 --threads=8
```

**Expected Outcome**:
- 1,000 queries complete with 100% success rate.
- Exact match hits + semantic vector hits resolve $> 25\%$ of queries.
- p95 latency on warm queries demonstrates $> 80\%$ reduction compared to cold upstream response times.
- Memory usage stays strictly $< 500\text{MB}$ RAM.
- Generates summary report with verified pass criteria.

---

### Scenario 3: High-Concurrency Throughput Benchmarking
Measures gateway throughput and latency percentiles under 200 concurrent worker threads.

```bash
# Run cache benchmark tool under load
./build/cache_benchmark --threads=200 --requests=10000
```

**Expected Outcome**:
- Sustained throughput $\ge 5,000\text{ RPS}$ for cached completions.
- Zero dropped calls or race conditions under sanitizers.
- p95 latency $< 2\text{ms}$ for Tier 1 and $< 20\text{ms}$ for Tier 2.

---

### Scenario 4: Containerized Stack Launch (Docker Compose)
Spins up the full system including Redis Stack and Tessera gateway in isolated containers.

```bash
# Build and launch multi-container stack
docker compose up -d --build

# Verify container health
docker compose ps
docker compose logs tessera-gateway

# Query Prometheus metrics endpoint
curl -s http://localhost:9090/metrics | head -n 30
```

**Expected Outcome**:
- `redis-stack` initializes with RediSearch and RedisJSON modules.
- `tessera-gateway` connects to Redis Stack and starts serving on ports 50051 and 9090.
- Health checks report `healthy` within 60 seconds.

---

### Scenario 5: Full CTest Matrix Run
Runs all unit, integration, concurrency, and rate-limiting test targets in parallel.

```bash
ctest --test-dir build --output-on-failure --parallel $(nproc)
```

**Expected Outcome**:
- 100% test targets pass with zero failures.

