# Research & Technical Decisions: Live Network Ingress (gRPC, OpenAI REST/SSE & Prometheus Metrics)

**Feature**: `010-live-network-ingress`  
**Date**: 2026-11-27  
**Scope**: Real gRPC / fallback TCP RPC listeners in `GatewayServer` and `main.cpp`, remote `gateway_client`, OpenAI HTTP/REST + SSE server (`HTTP_PORT=8080`), Prometheus HTTP endpoint (`METRICS_PORT=9090`), and CI/container alignment.

---

## Decision 1: Dual-Mode RPC Ingress (`TESSERA_HAS_SYSTEM_GRPC` vs. Fallback TCP RPC)

- **Decision**: Abstract the `GATEWAY_PORT` network transport behind a dedicated `RpcServer` / `RpcClient` layer (`include/tessera/server/rpc_server.hpp`, `src/server/rpc_server.cpp`, `include/tessera/server/rpc_client.hpp`, `src/server/rpc_client.cpp`):
  - When `TESSERA_HAS_SYSTEM_GRPC` is defined (`Protobuf` + `gRPC` found by CMake, such as Ubuntu CI), `RpcServer` runs a native multi-threaded asynchronous `grpc::Server` + `grpc::ServerCompletionQueue` loop handling `tessera.gateway.v1.GatewayService::AsyncService` (`ChatCompletion` and `Check`), and `RpcClient` uses `GatewayService::NewStub(grpc::CreateChannel(...))`.
  - When `TESSERA_HAS_SYSTEM_GRPC` is absent (e.g., minimal Docker/local builds without `libgrpc++-dev`), `RpcServer` binds a thread-pooled POSIX TCP listener using 4-byte length-prefixed JSON envelopes (`uint32_t network_len + UTF-8 JSON`) implementing the exact `ChatCompletion` and `Check` RPC contract, and `RpcClient` dials via TCP sockets using the identical framing.
  - Extend `protos/gateway.proto` with backward-compatible proto3 fields: `bool no_cache = 7` on `ChatCompletionRequest` and `bool cache_hit = 7; string cache_tier = 8` on `ChatCompletionResponse`.
- **Rationale**: Both Linux environments with system gRPC and lean builds without `libgrpc++-dev` gain true socket ingress (`GatewayServer::Start()` binds real TCP ports) and true remote client dialing (`gateway_client`) with identical logical contracts, payload-size enforcement, and ASan/UBSan safety.
- **Alternatives Considered**:
  - Requiring `libgrpc++-dev` unconditionally: Rejected because the existing build system treats gRPC as optional (`gRPC_DIR-NOTFOUND` locally and in `deploy/Dockerfile`) and the spec requires fallback TCP RPC interoperability when gRPC is absent.

---

## Decision 2: Reusable Multi-Threaded HTTP/1.1 Server for `HTTP_PORT` (`8080`) & `METRICS_PORT` (`9090`)

- **Decision**: Implement a single shared, zero-dependency, RAII HTTP/1.1 socket server (`HttpServer` in `include/tessera/server/http_server.hpp` and `src/server/http_server.cpp`) backed by `poll()` + a shutdown self-pipe for instant unblock on `Stop()`, `SO_REUSEADDR`, bounded worker threads, per-socket receive timeouts, `MSG_NOSIGNAL` writes (preventing `SIGPIPE` termination on aborted SSE reads), and strict `max_body_bytes` caps. Instantiate two instances inside `GatewayServer`:
  - **OpenAI REST/SSE Listener (`http_port = 8080`)**: Routes `POST /v1/chat/completions` (JSON + chunked `text/event-stream` SSE), `GET /v1/models`, and `GET /health`.
  - **Metrics Listener (`metrics_port = 9090`)**: Routes `GET /metrics` (`text/plain; version=0.0.4; charset=utf-8` from `TelemetryRegistry::Instance().ToPrometheusFormat()`) and `GET /health`.
  - If an operator explicitly configures `http_port == metrics_port` (`> 0`), dispatch both `/v1/*`, `/health`, and `/metrics` on one shared listener to avoid self-conflict; if a port is `0` while enabled (`enable_*_listener = true`), bind OS-assigned ephemeral ports exposed via `BoundPort()`, `BoundHttpPort()`, and `BoundMetricsPort()`.
- **Rationale**: Sharing one `HttpServer` engine avoids socket accept/parse/drain duplication (`code_quality`), isolates internal Prometheus scraping on `METRICS_PORT=9090` from OpenAI API traffic on `HTTP_PORT=8080` per Q1 Resolution (Option A), and gives parallel `ctest --parallel` runs zero-collision ephemeral ports.
- **Alternatives Considered**:
  - Adding an external HTTP library (e.g., Boost.Beast, cpp-httplib): Rejected to keep zero new third-party dependencies, full C++20/ASan/UBSan auditability, and seamless Docker multi-stage compilation.

---

## Decision 3: Self-Contained Recursive-Descent JSON Codec (`tessera/common/json.hpp`)

- **Decision**: Implement a clean, depth-bounded recursive-descent `JsonValue` parser and escaped serializer (`include/tessera/common/json.hpp`, `src/common/json.cpp`) supporting objects, arrays, strings (full escape decoding/encoding), numbers, booleans, and `null`. Reuse it across OpenAI JSON request parsing (`POST /v1/chat/completions`), OpenAI response & SSE chunk formatting, `/v1/models` rendering, OpenAI error payloads, and fallback TCP RPC envelopes.
- **Rationale**: Ad-hoc string searching (`find("\"content\":")`) cannot reliably validate malformed OpenAI payloads, nested `messages` arrays, whitespace variations, or escaped characters required by `FR-003`, `FR-009`, and strict error mapping (`400 Bad Request`).

---

## Decision 4: Progressive Chunked SSE Egress Over Unified Completion Pipeline (Q2 Option A)

- **Decision**: On `POST /v1/chat/completions` with `"stream": true`:
  1. Execute validation (`ValidateRequest`) and `GatewayServer::ProcessRequestSync` (honoring `"no_cache": true` or `Cache-Control: no-cache` headers and running `CachedCompletionBackend -> ModelRouter -> CascadeRouter`).
  2. Before writing SSE headers, map pre-stream errors strictly to JSON HTTP status codes (`400` invalid argument, `413` oversized payload, `429` local rate limit exhausted, `503` unavailable/draining).
  3. On `Status::OK()`, write HTTP `200 OK` + `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: close`, `X-Correlation-Id`, and `X-Tessera-Cache-Hit`, then flush incremental OpenAI `chat.completion.chunk` SSE events (`role: "assistant"` chunk -> progressive word/slice `delta.content` chunks whose concatenation equals the exact completion text -> final `finish_reason: "stop"` chunk -> `data: [DONE]\n\n`).
  4. Check write return values (`MSG_NOSIGNAL`) and abort loop cleanly on early client disconnect (`EPIPE`/`ECONNRESET`).
- **Rationale**: Resolves Q2 (Option A) cleanly—exact and semantic cache hits stream smoothly at high throughput, per-tier quotas and circuit breakers stay completely intact, and downstream OpenAI SDKs/clients receive spec-compliant SSE chunks without breaking cache writes.

---

## Decision 5: Ephemeral Ports & Zero-Collision Integration Testing + `gateway_server` Mock Mode

- **Decision**:
  - Set `ServerConfig` listener flags (`enable_rpc_listener = true`, `enable_http_listener = true`, `enable_metrics_listener = true`) defaulting to `port = 50051`, `http_port = 8080`, `metrics_port = 9090`. Update existing in-process integration tests (`test_server_async.cpp`, `test_graceful_shutdown.cpp`, `test_concurrency.cpp`, `test_validation_integration.cpp`) to configure `port = 0; http_port = 0; metrics_port = 0;` so every test runs real listeners on guaranteed-free kernel ephemeral ports even under `ctest --parallel $(nproc)`.
  - Add `--mock-upstreams` (or `TESSERA_MOCK_UPSTREAMS=1`) and `--http-port=` / `--metrics-port=` CLI options to `gateway_server` (`src/main.cpp`), plus `GatewayServer::SetModelListProvider(...)` populated from `ModelRouter::GetRegisteredModels()`.
  - Refactor `tools/gateway_client.cpp` into a strictly remote client dialing `GATEWAY_HOST`/`GATEWAY_PORT`, `HTTP_PORT`, and `METRICS_PORT` (`--host`, `--port`, `--http-port`, `--metrics-port`, `--health`, `--metrics`, `--message`, `--model`, `--no-cache`, `--http`, `--stream`, `--timeout-ms`), exiting non-zero when the server is unreachable or `NOT_SERVING`.
  - Update `.env.example`, `deploy/.env.example`, `Dockerfile`, `deploy/Dockerfile`, `deploy/docker-compose.yml`, `scripts/verify_metrics_format.sh`, and `.github/workflows/ci.yml` so offline smoke runs launch `gateway_server --mock-redis --mock-upstreams` cleanly and verify remote `gateway_client` + HTTP + SSE + `/metrics`.
- **Rationale**: Eliminates fake in-process client checks in Docker/CI while guaranteeing deterministic zero-collision CI test execution across compilers and sanitizers.
