# Quickstart & Local CI Simulation Guide

**Feature**: `006-ci-matrix-container-release`  
**Date**: 2026-11-14  
**Status**: Completed  

---

## 1. Prerequisites

Ensure a modern C++20 compiler (`clang++` or `g++`), CMake 3.25+, Ninja, and OpenSSL are installed:

```bash
# Ubuntu / Debian packages
sudo apt-get update && sudo apt-get install -y \
  cmake ninja-build build-essential libssl-dev clang
```

---

## 2. Local Simulation Scenarios

### Scenario 1: Simulate the Sanitizer Matrix Job (Clang + ASan/UBSan)
Validates that the full build, unit tests, integration tests, and benchmarks run cleanly without memory leaks or undefined behavior.

```bash
# Configure with Clang and Sanitizers
cmake -B build-ci -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

# Compile all targets
cmake --build build-ci --parallel $(nproc)

# Run test suite
ctest --test-dir build-ci --output-on-failure --parallel $(nproc)
```

**Expected Outcome**: 100% tests pass with 0 errors or leaks reported by AddressSanitizer.

---

### Scenario 2: Execute and Gate the 1,000-Query Evaluation Harness
Executes the synthetic evaluation harness and verifies all performance gates against the generated JSON report:

```bash
# Run 1,000-query benchmark
./build-ci/evaluation_harness --queries=1000 --threads=4 --output-json=eval_report.json

# Enforce CI SLA quality gates
python3 -c "
import json
with open('eval_report.json') as f:
    report = json.load(f)
assert report['failed_queries'] == 0, f'Failures: {report[\"failed_queries\"]}'
assert report['p95_latency_reduction_pct'] >= 80.0, f'p95 reduction: {report[\"p95_latency_reduction_pct\"]}%'
assert report['token_savings_pct'] >= 25.0, f'Token savings: {report[\"token_savings_pct\"]}%'
assert report['max_rss_mb'] < 500.0, f'Memory RSS: {report[\"max_rss_mb\"]}MB'
print('✓ CI Evaluation SLA Gates PASSED')
"
```

**Expected Outcome**: All assertions pass; script prints `✓ CI Evaluation SLA Gates PASSED`.

---

### Scenario 3: Verify Prometheus Metrics Text Exposition
Ensures telemetry formats properly conform to Prometheus standards:

```bash
# Query gateway client and inspect metrics exposition
./build-ci/gateway_client --metrics > metrics.txt

# Verify standard metric descriptors
grep -q "# HELP gateway_requests_total" metrics.txt && echo "✓ gateway_requests_total found"
grep -q "# HELP gateway_cache_hits_total" metrics.txt && echo "✓ gateway_cache_hits_total found"
grep -q "# HELP circuit_breaker_state" metrics.txt && echo "✓ circuit_breaker_state found"
grep -q "# HELP upstream_latency_ms" metrics.txt && echo "✓ upstream_latency_ms found"
```

**Expected Outcome**: All four metric descriptors are detected in the exposition output.

---

### Scenario 4: Container Build & Compose Smoke Test
When Docker is available locally, verify the multi-container stack:

```bash
# Build multi-stage production image
docker build -f deploy/Dockerfile -t tessera-gateway:ci .

# Start container stack
docker compose -f deploy/docker-compose.yml up -d

# Verify health status
docker compose -f deploy/docker-compose.yml ps

# Execute in-container health probe
docker exec tessera-gateway /usr/local/bin/gateway_client --health

# Clean shutdown
docker compose -f deploy/docker-compose.yml down -v
```

**Expected Outcome**: Stack enters healthy state; in-container health probe reports `Health Check: SERVING`.

