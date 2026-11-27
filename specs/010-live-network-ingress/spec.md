# Feature Specification: Live Network Ingress (gRPC, OpenAI REST/SSE & Prometheus Metrics)

**Feature Branch**: `010-live-network-ingress`  
**Created**: 2026-11-27  
**Status**: Draft  
**Input**: User description: "Live Network Ingress (gRPC, OpenAI REST/SSE & Prometheus Metrics): Wire real network server listeners into GatewayServer and main.cpp instead of in-process worker loops. Bind the native async gRPC server (and fallback TCP RPC transport when system gRPC is absent) on GATEWAY_PORT, and refactor gateway_client into a true remote client dialing the running server. Expose an HTTP server supporting OpenAI-compatible chat completion ingress (POST /v1/chat/completions with Server-Sent Events token streaming), model discovery (GET /v1/models), liveness/readiness probes (GET /health), and Prometheus exposition (GET /metrics on METRICS_PORT) while maintaining graceful connection draining, bounded memory, and ASan/UBSan cleanliness."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Real Async gRPC & Fallback TCP RPC Network Ingress + Remote `gateway_client` (Priority: P1) 🎯 MVP

As a distributed systems operator and service integrator, I want the gateway server to bind and accept real remote connections on `GATEWAY_PORT` using the native asynchronous gRPC server (or an interoperable length-framed TCP RPC listener when system gRPC libraries are absent at build time), and I want `gateway_client` to act as a true remote client dialing the running server rather than executing in-process, so that microservices, CLI tools, and container health checks validate and communicate with the live server daemon over the network.

**Why this priority**: Currently the server only runs idle worker sleep loops and the CLI binary executes an embedded in-process server—meaning zero network traffic actually traverses the gateway process and container health checks give false positives when the daemon is down or wedged.

**Independent Test**:
1. Launch `gateway_server` listening on `GATEWAY_PORT` with mock or configured upstream providers.
2. Execute `gateway_client --health`, `gateway_client --message "Hello remote gateway"`, and `gateway_client --no-cache --message "Bypass"` pointing at the running server host/port (and using `grpcurl` when built with system gRPC).
3. Verify `gateway_client` communicates over the network socket to receive live completion responses and server health state, and fails cleanly when the server is stopped or unreachable.

**Acceptance Scenarios**:
1. **Given** `gateway_server` is started on a configured host and `GATEWAY_PORT`,  
   **When** a remote RPC client (`gateway_client` or standard gRPC client when built with gRPC support) invokes chat completion or health check RPCs,  
   **Then** the gateway routes the network request through its full cache-first, quota-aware cascading pipeline and returns the response with a valid correlation ID over the wire.
2. **Given** the gateway binary is compiled in an environment without system gRPC development libraries,  
   **When** `gateway_server` starts and `gateway_client` dials `GATEWAY_PORT`,  
   **Then** both interoperate cleanly via the fallback TCP RPC transport supporting completion requests and health checks without requiring system gRPC installation.
3. **Given** no `gateway_server` process is listening on `GATEWAY_PORT` (or the server enters `NOT_SERVING` drain state),  
   **When** `gateway_client --health` or a completion request is executed against that endpoint,  
   **Then** `gateway_client` reports an unreachable or not-serving failure and exits non-zero rather than spinning up a hidden local server.

---

### User Story 2 - OpenAI-Compatible HTTP/REST Ingress & SSE Token Streaming (Priority: P1)

As an application developer integrating standard LLM SDKs (OpenAI Python/TypeScript SDKs, `curl`, web apps), I want an HTTP endpoint compatible with OpenAI chat completion and model listing APIs (`POST /v1/chat/completions` supporting both standard JSON responses and `text/event-stream` Server-Sent Events streaming, plus `GET /v1/models`), so that existing client applications can talk directly to Tessera as a drop-in OpenAI-compatible gateway over standard HTTP.

**Why this priority**: Nearly all downstream LLM applications and SDKs default to HTTP JSON and Server-Sent Events (`/v1/chat/completions`, `/v1/models`); supporting standard OpenAI REST + SSE unlocks immediate drop-in adoption alongside gRPC.

**Independent Test**:
1. Start `gateway_server` with configured providers and models.
2. Query `GET /v1/models`, `POST /v1/chat/completions` (`stream: false`), and `POST /v1/chat/completions` (`stream: true`) using a standard HTTP client.
3. Verify `/v1/models` returns an OpenAI-formatted model list containing registered routes, non-streaming completions return valid OpenAI JSON payloads (with cache-bypass header/field support), and streaming completions emit incremental `data: {...}` SSE events terminated by `data: [DONE]`.

**Acceptance Scenarios**:
1. **Given** active providers and models registered in the gateway catalog,  
   **When** a client calls `GET /v1/models`,  
   **Then** the HTTP server responds `200 OK` (`application/json`) listing active models in standard OpenAI model-list format.
2. **Given** a valid OpenAI-formatted JSON body on `POST /v1/chat/completions` with `"stream": false` (or omitted),  
   **When** the gateway processes the request,  
   **Then** it returns `200 OK` (`application/json`) containing an OpenAI completion object (`id`, `object`, `created`, `model`, `choices`, `usage`) alongside cache/correlation headers, or maps validation/quota/upstream errors cleanly to HTTP `400`/`429`/`503` JSON error payloads.
3. **Given** a valid `POST /v1/chat/completions` request with `"stream": true`,  
   **When** the completion is resolved through the cache-first, quota-aware cascade pipeline,  
   **Then** the HTTP server responds with `Content-Type: text/event-stream`, flushes incremental `data: <chunk JSON>\n\n` events matching OpenAI streaming chunk schema by progressively chunking the resolved completion text, and terminates cleanly with `data: [DONE]\n\n`.

---

### User Story 3 - Prometheus Exposition (`GET /metrics` on `METRICS_PORT`) & HTTP Health Probes (`GET /health`) (Priority: P1)

As an infrastructure SRE and container orchestrator, I want `gateway_server` to serve live Prometheus telemetry text format on `GET /metrics` bound to `METRICS_PORT` and HTTP readiness/liveness status on `GET /health`, so that Prometheus scrapers, Kubernetes readiness probes, and Compose health checks monitor real live traffic and immediately detect draining or degraded states.

**Why this priority**: Real observability and zero-downtime orchestration depend on live network `/metrics` scraping and HTTP `/health` transitions (`200 SERVING` vs `503 NOT_SERVING`) reflecting true server state.

**Independent Test**:
1. Start `gateway_server` and send a few completion requests (cache miss + cache hit).
2. Fetch `GET /metrics` on `METRICS_PORT` and `GET /health` via HTTP.
3. Verify `/metrics` returns HTTP `200` (`text/plain; version=0.0.4`) with live incremented counters/histograms matching `verify_metrics_format.sh`, and `/health` returns `200 OK` while serving and `503 Service Unavailable` once shutdown draining begins.

**Acceptance Scenarios**:
1. **Given** `gateway_server` is serving traffic,  
   **When** Prometheus scrapes `GET /metrics` on `METRICS_PORT`,  
   **Then** the server returns `200 OK` with Prometheus 0.0.4 exposition text reflecting live `gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`, and `gateway_ratelimit_skips_total`.
2. **Given** both OpenAI REST/SSE endpoints and Prometheus metrics exposition are active on `gateway_server`,  
   **When** default ports are used (or configured via `HTTP_PORT` and `METRICS_PORT`),  
   **Then** OpenAI REST/SSE ingress (`/v1/chat/completions`, `/v1/models`) and `/health` bind on `HTTP_PORT` (default `8080`), while Prometheus `/metrics` (and `/health`) bind on `METRICS_PORT` (default `9090`), isolating internal metrics scraping from public client API traffic without port conflicts.
3. **Given** `gateway_server` is healthy vs. initiating graceful shutdown drain,  
   **When** a probe queries `GET /health`,  
   **Then** the endpoint returns HTTP `200` (`SERVING`) during normal operation and HTTP `503` (`NOT_SERVING`) immediately upon entering drain mode.

---

### User Story 4 - Graceful Connection Draining, Bounded Memory & Sanitizer Cleanliness (Priority: P2)

As a platform reliability engineer, I want the live network listeners (gRPC/TCP RPC and HTTP servers) to reject new requests cleanly during shutdown, finish in-flight RPC and HTTP/SSE responses within the drain timeout, enforce payload size limits, and free all connection/thread resources cleanly without leaks or data races under concurrent traffic, so that deployments upgrade seamlessly with zero dropped in-flight responses and full memory safety.

**Why this priority**: Network socket servers introduce connection lifetimes, partial socket reads, client disconnects during SSE streaming, and listener shutdown races that must stay strictly leak-free and deadlock-free under load and ASan/UBSan.

**Independent Test**:
1. Dispatch concurrent HTTP REST, SSE streaming, `/metrics` scrapes, and remote RPC calls while triggering `GatewayServer::Stop()` mid-flight.
2. Verify in-flight requests accepted before `Stop()` finish cleanly, post-stop connection attempts fail fast (`503` / `UNAVAILABLE`), sockets close cleanly, and sanitizer integration suites pass with zero memory leaks or undefined behavior.

**Acceptance Scenarios**:
1. **Given** active in-flight HTTP and RPC completion requests across multiple client threads,  
   **When** graceful shutdown is initiated,  
   **Then** listeners stop accepting new work (or respond unavailable immediately), existing in-flight requests run up to completion within the configured drain timeout, and all listener threads join cleanly.
2. **Given** a client closes a TCP/SSE connection prematurely or sends malformed HTTP/RPC framing or an oversized payload exceeding the configured max message limit,  
   **When** the server processes the socket event,  
   **Then** it returns an actionable `400`/`413` error (or aborts cleanly on EOF), releases all per-connection resources cleanly without crashing, and decrements active in-flight tracking accurately.
3. **Given** all automated unit, integration, concurrency, and network ingress tests run under memory and undefined-behavior sanitizers,  
   **When** executed sequentially and under load,  
   **Then** 100% pass with zero leaks, dangling sockets, port bind conflicts (via ephemeral test ports), or sanitizer warnings.

---

### Edge Cases

- **Port Already in Use / Bind Failure**: If `GATEWAY_PORT`, `METRICS_PORT`, or HTTP ingress port fails to bind at startup, server startup MUST fail cleanly (returning `false` / non-zero exit with a clear diagnostic log) and tear down any already-bound listeners cleanly without leaving partial sockets or leaked threads.
- **Keep-Alive / Short-Lived Clients & Partial Reads/Writes**: HTTP clients sending fragmented TCP packets, `Content-Length` bodies across multiple `recv()` calls, `Connection: close`, or HTTP/1.1 persistent connections MUST be parsed accurately without hanging waiting for extra bytes or truncating payloads.
- **Malformed JSON or Missing OpenAI Fields**: Invalid JSON or requests missing required `model` or `messages` fields on `POST /v1/chat/completions` MUST return HTTP `400 Bad Request` in OpenAI error JSON format (`{"error":{"message":...,"type":"invalid_request_error",...}}`).
- **Rate-Limit Exhaustion vs Upstream Outage HTTP Mapping**: When all providers in a cascade are exhausted, local rate-limit exhaustion MUST map cleanly to HTTP `429 Too Many Requests` (preserving retry-after guidance where present), whereas upstream/open-circuit exhaustion maps cleanly to HTTP `503 Service Unavailable`.
- **Cache Bypass via HTTP**: Setting `"no_cache": true` or `Cache-Control: no-cache` header on `POST /v1/chat/completions` MUST trigger cache bypass identically to gRPC/internal pipeline requests.
- **Remote `gateway_client` Modes (`--health`, `--metrics`, `--message`, `--http`, `--stream`)**:
  - `--health`: Dials the running server's health RPC (or HTTP `/health`) and exits `0` iff `SERVING`.
  - `--metrics`: Queries live `GET /metrics` from the running server's `METRICS_PORT` (after sending `--message` if supplied, or standalone) rather than printing empty local process metrics.
  - `--message` / `--model` / `--no-cache` / `--http` / `--stream`: Sends live RPC (`GATEWAY_PORT`) or OpenAI HTTP/SSE (`HTTP_PORT`) requests to the running `gateway_server` (`--host`/`--port`/`--http-port` or `GATEWAY_HOST`/`GATEWAY_PORT`/`HTTP_PORT`); mock upstream/failover smoke testing runs server-side via `gateway_server --mock-upstreams`.
  - Dual-build compatibility: Both system-gRPC builds (`TESSERA_HAS_SYSTEM_GRPC`) and fallback-TCP builds compile and communicate seamlessly between `gateway_server` and `gateway_client`.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST replace the idle worker sleep loop in `GatewayServer` and wire real multi-threaded network listeners started cleanly by `GatewayServer::Start()` and stopped gracefully by `GatewayServer::Stop()`.
- **FR-002**: System MUST bind a real RPC listener on `GATEWAY_PORT` supporting asynchronous `GatewayService` (`ChatCompletion` and `Check`) over native async gRPC when built with system gRPC libraries, and an interoperable TCP RPC server transport when system gRPC is unavailable at build time.
- **FR-003**: System MUST expose an HTTP server on `HTTP_PORT` (default `8080`) supporting OpenAI-compatible `POST /v1/chat/completions` (returning OpenAI JSON or progressive `text/event-stream` Server-Sent Events chunks with `[DONE]` termination when `"stream": true`), `GET /v1/models`, and `GET /health` (`200 SERVING` / `503 NOT_SERVING`).
- **FR-004**: System MUST expose a dedicated Prometheus HTTP server on `METRICS_PORT` (default `9090`) serving live `0.0.4` text format on `GET /metrics` and readiness/liveness state on `GET /health`.
- **FR-005**: System MUST route all incoming gRPC, fallback TCP RPC, and HTTP REST/SSE completion requests through `GatewayServer`'s cache-first, quota-aware request pipeline while enforcing configured maximum payload limits, correlation IDs, cache-bypass controls, and thread-safe in-flight request tracking.
- **FR-006**: System MUST enforce graceful shutdown draining across all network listeners: transitioning health status to `NOT_SERVING` (`503`), rejecting post-drain requests, waiting up to the configured drain timeout for in-flight requests to complete, closing listening sockets cleanly to unblock acceptors/pollers, and joining all network threads cleanly.
- **FR-007**: System MUST refactor `gateway_client` into a true remote network client dialing a live running server host/port (`GATEWAY_HOST`/`GATEWAY_PORT`, `HTTP_PORT`, and `METRICS_PORT`, with CLI overrides such as `--host`, `--port`, `--http-port`, `--metrics-port`, `--http`, and `--stream`) for completion requests, health probes, and remote `/metrics` fetching—eliminating hidden in-process server instantiation during normal client operation.
- **FR-008**: System MUST align server entrypoints, `.env.example`, container definitions (`Dockerfile`, `deploy/Dockerfile`, `deploy/docker-compose.yml` exposing `50051`, `8080`, `9090`), `scripts/verify_metrics_format.sh`, and CI workflows (`.github/workflows/ci.yml`) so server startup, container health checks, and automated Prometheus/CLI validation run cleanly against live network listeners (supporting offline/mock upstream startup flags where live API keys are absent).
- **FR-009**: System MUST handle malformed payloads, oversized requests, abrupt client disconnects, and ephemeral port binding (`port = 0` or dynamically assigned ports in tests) cleanly without leaks, crashes, or port collisions.
- **FR-010**: System MUST compile without warnings under strict C++20 compiler flags (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`) with or without system gRPC, and pass all unit, integration, concurrency, and network ingress tests with zero errors or leaks under AddressSanitizer and UndefinedBehaviorSanitizer.

### Key Entities

- **Network RPC Ingress (`GATEWAY_PORT`)**: Live socket listener dispatching unary chat completions and health checks through `GatewayServer`, backed by native async gRPC (`CompletionQueue` state machine) when available or a framed TCP RPC server transport when system gRPC is absent.
- **HTTP / OpenAI Ingress (`POST /v1/chat/completions`, `GET /v1/models`, `GET /health`)**: Non-blocking / thread-pooled HTTP/1.1 server translating OpenAI JSON payloads, `Cache-Control` / `"no_cache"` flags, and SSE stream frames cleanly to and from unified gateway requests/responses and HTTP error statuses.
- **Metrics Exposition Endpoint (`GET /metrics` on `METRICS_PORT`)**: Live HTTP endpoint serving `TelemetryRegistry` Prometheus 0.0.4 metrics reflecting real-time gateway completions, cache tiers, circuit states, upstream latencies, and local rate-limit skips.
- **Remote CLI Client (`gateway_client`)**: Thin network client dialing remote gRPC / TCP RPC, HTTP REST/SSE, and `/metrics` endpoints on a running `gateway_server` instance.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: **Real Network End-to-End Completions & Streaming**: External clients (`gateway_client`, standard HTTP requests, and `grpcurl` when built with gRPC) receive valid completions from a live running `gateway_server` over `GATEWAY_PORT` and HTTP, including SSE token streaming (`data: [DONE]` termination) with `0` embedded server hacks in `gateway_client`.
- **SC-002**: **Live Health & Prometheus Scrape Fidelity**: Querying `/health` returns HTTP `200` while serving and transitions to HTTP `503` immediately during shutdown draining; scraping `GET /metrics` on `METRICS_PORT` reflects 100% of live network requests and cache hits accurately and passes `scripts/verify_metrics_format.sh`.
- **SC-003**: **Zero-Drop Graceful Drain**: Active in-flight network requests started prior to shutdown complete `100%` within the drain window before server exit, while newly arriving requests post-drain are rejected cleanly (`503`/`UNAVAILABLE`).
- **SC-004**: **Cross-Transport Build Portability**: Both system-gRPC builds (`TESSERA_HAS_SYSTEM_GRPC`) and gRPC-free TCP fallback builds compile with `0` warnings and pass 100% of unit, integration, concurrency, and network ingress tests.
- **SC-005**: **Bounded Memory & Sanitizer Cleanliness**: Multi-threaded network load, malformed HTTP/RPC fuzz inputs, and premature SSE disconnects run with `0` memory leaks, `0` use-after-free/race issues, and `0` undefined behavior diagnostics under ASan and UBSan.

---

## Assumptions

- Both native async gRPC (when `Protobuf` + `gRPC` packages are found at CMake configure time) and the self-contained TCP fallback transport share identical logical semantics and are selected transparently at build time based on `CMakeLists.txt` dependency detection.
- **Runtime listener configuration** includes settings for `GATEWAY_PORT` (default `50051`), `HTTP_PORT` (default `8080`), and `METRICS_PORT` (default `9090`), honoring `0` for OS-assigned ephemeral ports or disabled listeners when automated tests specify isolation.
- When `"stream": true` is requested on `POST /v1/chat/completions`, progressively chunking the resolved/cached completion returned by the gateway pipeline over SSE preserves exact/semantic cache preemption, distributed quota accounting, and multi-provider failover semantics uniformly without altering upstream provider adapters.
- Adding an offline mock-upstream startup mode flag (e.g., `--mock-upstreams` alongside `--mock-redis`) on `gateway_server` enables hermetic CI, Compose smoke tests, and local validation without requiring external LLM API keys or local Ollama daemons.
- TLS termination at the gateway listener edge (HTTPS/mTLS ingress) is assumed handled by an upstream reverse proxy / load balancer or deferred outside this feature unless explicitly requested, keeping plain HTTP/1.1 + gRPC/TCP clean and hermetic.
