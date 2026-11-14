# CI Workflow Contract: GitHub Actions Matrix & Container Pipeline

**Feature**: `006-ci-matrix-container-release`  
**Date**: 2026-11-14  
**Status**: Completed  

---

## 1. Workflow Metadata & Triggers

The workflow file is located at `.github/workflows/ci.yml`.

### Triggers
```yaml
on:
  push:
    branches: [ master, main ]
  pull_request:
    branches: [ master, main ]
  workflow_dispatch:
```

---

## 2. Job 1: `build-and-test` (Matrix Job)

### Matrix Dimensions
```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      - compiler: gcc
        cc: gcc
        cxx: g++
        sanitizer: none
        build_type: Release
      - compiler: clang
        cc: clang
        cxx: clang++
        sanitizer: address,undefined
        build_type: Debug
```

### Services
```yaml
services:
  redis:
    image: redis/redis-stack-server:latest
    ports:
      - 6379:6379
```

### Required Step Execution Order
1. **Checkout Code**: `actions/checkout@v4`
2. **Install System Dependencies**:
   `cmake ninja-build build-essential libssl-dev libprotobuf-dev protobuf-compiler libgrpc++-dev protobuf-compiler-grpc`
3. **Setup Compiler Environment**: Set `CC` and `CXX`, print versions.
4. **Configure CMake**:
   `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} -DBUILD_TESTS=ON [SAN_FLAG]`
5. **Build All Targets**:
   `cmake --build build --config ${{ matrix.build_type }} --parallel $(nproc)`
6. **Run Unit & Integration Tests**:
   `ctest --test-dir build --output-on-failure --parallel $(nproc)`
7. **Run High-Concurrency Load Test (500 clients)**:
   `./build/concurrency_test`
8. **Run Rate Limit Concurrency Test (200 threads)**:
   `./build/ratelimit_concurrency_test`
9. **Run Exact Cache Benchmark (10,000 queries)**:
   `./build/cache_benchmark --iterations=10000`
10. **Run Semantic Cache Benchmark (1,000 queries)**:
    `./build/semantic_benchmark --iterations=1000`
11. **Run Synthetic 1,000-Query Evaluation Harness**:
    `./build/evaluation_harness --queries=1000 --threads=4 --output-json=eval_report.json`
12. **Enforce Evaluation SLA Gates**:
    Python / shell verification script asserting:
    - `report["failed_queries"] == 0`
    - `report["p95_latency_reduction_pct"] >= 80.0`
    - `report["token_savings_pct"] >= 25.0`
    - `report["max_rss_mb"] < 500.0`
13. **Validate Prometheus Metrics Exposition**:
    `./build/gateway_client --metrics` asserting metric names and descriptors.
14. **Upload Evaluation Benchmark Artifact**:
    `actions/upload-artifact@v4` uploading `eval_report.json` with artifact name `evaluation-report-${{ matrix.compiler }}`.

---

## 3. Job 2: `container-smoke-test` (Docker Compose Validation)

### Prerequisites
Runs on `ubuntu-latest`.

### Steps
1. **Checkout Code**: `actions/checkout@v4`
2. **Build Production Image**:
   `docker build -f deploy/Dockerfile -t tessera-gateway:ci .`
3. **Launch Multi-Container Stack**:
   `docker compose -f deploy/docker-compose.yml up -d`
4. **Wait for Container Health**:
   Wait up to 45 seconds for both `redis-stack` and `tessera-gateway` to become healthy.
5. **Execute In-Container Health Check**:
   `docker exec tessera-gateway /usr/local/bin/gateway_client --health`
6. **Execute Test Query with Metrics**:
   `docker exec tessera-gateway /usr/local/bin/gateway_client --message "Docker CI verification query" --metrics`
7. **Tear Down Stack**:
   `docker compose -f deploy/docker-compose.yml down -v`

