# Quickstart Guide: gRPC Gateway Foundation

**Feature**: `001-grpc-gateway-foundation`  
**Date**: 2026-11-01  
**Status**: Ready for Verification  

---

## Prerequisites

* **Operating System**: Linux (Ubuntu 22.04+ or Debian 12+)
* **Compiler**: GCC 13+ (`g++-13`) or Clang 17+ (`clang++-17`)
* **Build Tools**: CMake 3.25+, Ninja build system (`ninja-build`)
* **Core Libraries**: `libgrpc++-dev`, `protobuf-compiler-grpc`, `libprotobuf-dev`

```bash
# Ubuntu / Debian installation
sudo apt-get update
sudo apt-get install -y cmake ninja-build build-essential \
  libprotobuf-dev protobuf-compiler libgrpc++-dev protobuf-compiler-grpc
```

---

## Build Instructions

```bash
# 1. Configure the build directory using Ninja with tests enabled
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON

# 2. Build the gateway server and test suites
cmake --build build --parallel $(nproc)
```

To build with AddressSanitizer and UndefinedBehaviorSanitizer:
```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## Verification Scenarios

### Scenario 1: Run the Automated Unit Test Suite
Execute the GoogleTest suite to verify protocol serialization, input validation, and asynchronous completion queue operations:

```bash
ctest --test-dir build --output-on-failure
```
**Expected Outcome**: All tests pass (`100% tests passed, 0 tests failed out of N`).

---

### Scenario 2: Start the Gateway Server
Launch the asynchronous gRPC gateway server on port 50051:

```bash
./build/bin/gateway_server --port=50051 --threads=4
```
**Expected Outcome**:
```text
[INFO] Starting Tessera Gateway on 0.0.0.0:50051 with 4 worker threads...
[INFO] Gateway service listening and healthy.
```

---

### Scenario 3: Submit Chat Completion & Health Probe
Using the included test client binary:

```bash
# Verify health check
./build/bin/gateway_client --health

# Verify chat completion
./build/bin/gateway_client --message "Explain distributed systems in one sentence."
```
**Expected Outcome**:
* Health probe returns: `Status: SERVING`
* Chat completion returns a valid `ChatCompletionResponse` containing generated message, `id: chatcmpl-...`, and token usage counts.

---

### Scenario 4: Concurrency Load Test (500 Concurrent Requests)
Run the concurrent load simulator to verify non-blocking async throughput:

```bash
./build/bin/concurrency_test --clients=500 --total=5000
```
**Expected Outcome**:
* 100% completion rate (0 drops or timeouts).
* p95 latency overhead $< 2\text{ms}$.

