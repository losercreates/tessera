# Quickstart Guide: Local ONNX Semantic Cache & Vector Search

**Feature**: `003-onnx-semantic-cache`  
**Date**: 2026-11-03  
**Status**: Ready for Implementation  

---

## Prerequisites

* **Operating System**: Linux (Ubuntu 22.04+ or Debian 12+)
* **Compiler**: GCC 13+ or Clang 17+ (C++20 compliant)
* **Build System**: CMake 3.25+ and Ninja
* **System Libraries**: `libssl-dev`, `build-essential`

---

## Build Instructions

```bash
# Configure and build release binaries
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

With AddressSanitizer and UndefinedBehaviorSanitizer enabled:
```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## Verification Scenarios

### Scenario 1: Paraphrased Query Hit in Tier 2 Semantic Cache (< 20ms)
1. Prime the cache with an initial prompt:
   ```bash
   ./build/gateway_client --message "What is the capital city of France?"
   ```
2. Query with a semantically equivalent paraphrase:
   ```bash
   ./build/gateway_client --message "Tell me what France's capital is."
   ```
   **Expected Output**:
   * `Cache Hit:      true`
   * `Cache Tier:     semantic`
   * Latency $< 20\text{ms}$ with zero upstream provider tokens consumed.

---

### Scenario 2: Two-Tier Hierarchical Coordination
1. Submit an identical duplicate prompt:
   ```bash
   ./build/gateway_client --message "What is the capital city of France?"
   ```
   **Expected Output**:
   * `Cache Hit:      true`
   * `Cache Tier:     exact`
   * Latency $< 2\text{ms}$ (embedding computation bypassed).

---

### Scenario 3: Semantic Cache Benchmark (Paraphrased Queries)
Run the automated semantic cache benchmark measuring p95 latency and hit rates:

```bash
./build/semantic_benchmark --iterations=1000
```
**Expected Output**:
* p95 Semantic Cache Hit Latency $< 20\text{ms}$.
* 100% hit rate for paraphrased queries with cosine similarity $\ge 0.95$.
* 0% false positive hit rate for semantically distinct queries.

---

### Scenario 4: Selective Bypass (`--no-cache`) & Fail-Open Degradation
1. Bypass semantic cache:
   ```bash
   ./build/gateway_client --message "What is the capital city of France?" --no-cache
   ```
   **Expected Output**:
   * `Cache Hit:      false`
   * Upstream completion is executed, and both cache tiers are updated.

