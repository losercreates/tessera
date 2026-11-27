# Quickstart & Validation Guide: Live Network Ingress

**Feature**: `010-live-network-ingress`  
**Date**: 2026-11-27  
**Status**: Verified (Debug + ASan/UBSan clean build, 4/4 CTest suites, `verify_metrics_format.sh`, remote `gateway_client`, and live `curl` REST/SSE/`/v1/models`/`/health`/`/metrics` checks passed)

---

## 1. Prerequisites

Build all gateway targets and tests with ASan and UBSan enabled:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## 2. End-to-End Validation Scenarios

### Scenario 1: Live `gateway_server` + Remote `gateway_client` & OpenAI REST/SSE Ingress
Starts `gateway_server` listening on `GATEWAY_PORT` (`50051`), `HTTP_PORT` (`8080`), and `METRICS_PORT` (`9090`) in offline hermetic mock mode (`--mock-redis --mock-upstreams`), then exercises remote RPC, OpenAI REST/SSE HTTP, `/v1/models`, `/health`, and `/metrics`.

```bash
./build/gateway_server --mock-redis --mock-upstreams --port=15051 --http-port=18080 --metrics-port=19090 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null' EXIT

# Remote health probe & remote RPC completion via gateway_client
./build/gateway_client --host=127.0.0.1 --port=15051 --health
./build/gateway_client --host=127.0.0.1 --port=15051 --metrics-port=19090 \
  --message="What is distributed consensus?" --metrics

# OpenAI REST /v1/models, non-streaming, streaming SSE, /health, /metrics
curl -sf http://127.0.0.1:18080/health
curl -sf http://127.0.0.1:18080/v1/models
curl -sf -X POST http://127.0.0.1:18080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"auto","messages":[{"role":"user","content":"Hello OpenAI REST"}]}'
curl -sN -X POST http://127.0.0.1:18080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"auto","messages":[{"role":"user","content":"Stream tokens"}],"stream":true}'
curl -sf http://127.0.0.1:19090/metrics
```

**Expected Outcome**:
- Remote `gateway_client --health` exits `0` (`SERVING`) only while `gateway_server` is up (exiting `1` once stopped).
- `/v1/models` lists registered active model routes in OpenAI format; non-streaming returns HTTP `200` JSON; SSE streaming flushes `data: {...}` chunks ending with `data: [DONE]`.
- `GET /metrics` reflects live counters/histograms across both remote RPC and OpenAI HTTP calls.

---

### Scenario 2: Prometheus Verification Gate & CTest ASan/UBSan Suites
Validates live network `unit_tests`, `integration_tests` (covering RPC loopback, REST/SSE, `/v1/models`, `/health`, `/metrics`, malformed/oversized payloads, and concurrent graceful draining), concurrency suites, and `verify_metrics_format.sh`.

```bash
ctest --test-dir build --output-on-failure
bash scripts/verify_metrics_format.sh
```

**Expected Outcome**:
- 100% pass rate across all CTest binaries (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`) under ASan/UBSan with 0 memory leaks, socket leaks, or compiler warnings.
- `scripts/verify_metrics_format.sh` passes all Prometheus 0.0.4 checks against live HTTP `/metrics`.
