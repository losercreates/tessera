# Implementation Plan: Live Network Ingress (gRPC, OpenAI REST/SSE & Prometheus Metrics)

**Branch**: `010-live-network-ingress` | **Date**: 2026-11-27 | **Spec**: [spec.md](file:///home/mayukh/projects/tessera/specs/010-live-network-ingress/spec.md)  
**Input**: Feature specification from `/specs/010-live-network-ingress/spec.md`

---

## Summary

Replace `GatewayServer`'s in-process sleep worker loop with real multi-threaded network listeners spanning both **RPC ingress on `GATEWAY_PORT` (`50051`)** (native asynchronous gRPC `ServerCompletionQueue` when built with `TESSERA_HAS_SYSTEM_GRPC`, and an interoperable length-prefixed JSON TCP RPC server when system gRPC is absent) and **HTTP/1.1 servers** built on one reusable `poll()` + self-pipe `HttpServer` engine exposing **OpenAI-compatible REST & chunked SSE streaming (`POST /v1/chat/completions`, `GET /v1/models`, `GET /health`) on `HTTP_PORT` (`8080`)** and **Prometheus `GET /metrics` + `GET /health` on `METRICS_PORT` (`9090`)**. Refactor `gateway_client` into a true remote client dialing live servers (`RpcClient` + remote HTTP `/metrics` & OpenAI REST/SSE), extend `main.cpp` and `ServerConfig`, and align `.env.example`, `Dockerfile` / `deploy/Dockerfile`, `deploy/docker-compose.yml`, `scripts/verify_metrics_format.sh`, and `.github/workflows/ci.yml` while keeping 100% CTest pass rate under ASan and UBSan.

---

## Technical Context

**Language/Version**: C++20 (`-std=gnu++20`, `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`)  
**Primary Dependencies**: POSIX sockets (`sys/socket.h`, `poll.h`, `netinet/in.h`, `arpa/inet.h`), STL threads/atomics/mutexes/cv, OpenSSL (existing client crypto), optional Protobuf + gRPC++ (`TESSERA_HAS_SYSTEM_GRPC=1` when found in CMake)  
**Storage**: Reuses existing two-tier Redis/in-memory cache & rate-limiter (`MockRedisClient` / `RedisConnectionPool`) and `TelemetryRegistry`; zero external HTTP/JSON third-party dependencies added  
**Testing**: CTest (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`) under Debug + ASan/UBSan (`-fsanitize=address,undefined`), plus `scripts/verify_metrics_format.sh` and live client/HTTP/SSE smoke validation  
**Target Platform**: Linux (x86_64/arm64, CMake 3.25+ Ninja with GCC/Clang; works identically with or without `libgrpc++-dev`)  
**Project Type**: Native C++20 API gateway library (`tessera_core`), server daemon (`gateway_server`), and remote CLI client (`gateway_client`)  
**Performance Goals**: Instant (`<1ms` wakeup via self-pipe + `poll()`) socket listener shutdown; sub-millisecond ephemeral socket dispatch; zero `SIGPIPE` aborts on broken SSE/HTTP connections  
**Constraints**: Strict C++20 RAII (`std::unique_ptr`/`std::shared_ptr`/RAII FD guards), bounded HTTP/RPC payload enforcement (`max_message_size`), graceful drain within `graceful_drain_timeout_ms`, ephemeral ports (`0`) in parallel tests to eliminate `EADDRINUSE` races  
**Scale/Scope**: Adds modular `common/json.*`, `server/http_server.*`, `server/rpc_server.*`, `server/rpc_client.*`; updates `gateway_server.*`, `server_config.hpp`, `protos/gateway.proto`, `src/main.cpp`, `tools/gateway_client.cpp`, existing async/shutdown/concurrency tests (`port = 0`), deployment/CI scripts, and new unit + live network integration tests

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Design Check | Post-Design Check | Notes |
|---|---|---|---|
| **I. Modern C++20 Architecture & Async Concurrency** | PASS | PASS | Uses RAII socket guards, smart pointers, async gRPC `CompletionQueue` / non-blocking `poll()` listeners, and clean layer boundaries (`json`, `http_server`, `rpc_server`, `rpc_client`). |
| **II. Determinism, Reliability & Fault-Tolerant Routing** | PASS | PASS | All network endpoints dispatch through cache-first `ModelRouter`/`CascadeRouter`, enforce deterministic HTTP `400`/`413`/`429`/`503` and gRPC status codes, and drain gracefully on shutdown. |
| **III. Embedded Local Semantic Inference & Two-Tier Caching** | PASS | PASS | Preserves Tier 1/Tier 2 cache preemption, `no_cache` & `Cache-Control: no-cache` bypass, and progressive SSE chunking over unified pipeline responses. |
| **IV. Strict Quality Gate & Testing Mandate** | PASS | PASS | Adds unit tests (`test_json.cpp`) and live loopback integration tests (`test_live_ingress.cpp`) passing under ASan/UBSan alongside existing suites using ephemeral ports (`0`). |
| **V. CI/CD Reproducibility, Tooling & Observability** | PASS | PASS | Strict compiler warning cleanliness across both gRPC and non-gRPC builds; real live `GET /metrics` on `METRICS_PORT=9090`; CI & Docker Compose updated to dial the live server. |

---

## Project Structure

### Documentation (this feature)

```text
specs/010-live-network-ingress/
├── plan.md                                  # This file
├── research.md                              # Phase 0 architectural & dual-transport decisions
├── data-model.md                            # Phase 1 payloads, TCP RPC framing, & drain state machine
├── quickstart.md                            # Phase 1 end-to-end validation scenarios
├── contracts/
│   └── live_ingress_contract.md             # Phase 1 C++, Protobuf, HTTP/SSE, & CLI contracts
└── tasks.md                                 # Phase 2 output (/speckit-tasks command)
```

### Source Code (repository root)

```text
protos/
└── gateway.proto                            # Add no_cache (req) and cache_hit, cache_tier (resp)

include/tessera/
├── common/json.hpp                          # Self-contained JSON AST, parser & escaped serializer
└── server/
    ├── server_config.hpp                    # Add http_port=8080, metrics_port=9090, enable_* flags
    ├── gateway_server.hpp                   # Own RpcServer + Ingress/Metrics HttpServers, Bound*Port(), ListModels()
    ├── http_server.hpp                      # Reusable poll()-based HTTP/1.1 + SSE server
    ├── rpc_server.hpp                       # Unified RPC listener (Async gRPC or TCP JSON frame)
    └── rpc_client.hpp                       # Remote RPC & HTTP/SSE client helper

src/
├── main.cpp                                 # Parse HTTP_PORT, METRICS_PORT, --mock-upstreams, wire ModelListProvider
├── common/json.cpp                          # JSON recursive-descent parser & serializer
└── server/
    ├── gateway_server.cpp                   # Start/Stop real listeners, track in-flight drainage cleanly
    ├── http_server.cpp                      # HTTP/1.1 + SSE streaming implementation (MSG_NOSIGNAL, max_body_bytes)
    ├── rpc_server.cpp                       # Async gRPC CompletionQueue loop OR TCP RPC worker loop
    └── rpc_client.cpp                       # Remote gRPC/TCP RPC + HTTP REST/SSE + /metrics fetcher

tools/
└── gateway_client.cpp                       # Remote-only CLI dialing live server (--health, --metrics, --message, --http, --stream)

tests/
├── unit/
│   └── test_json.cpp                        # JSON roundtrip, unicode/escapes, malformed rejection, OpenAI schema
└── integration/
    ├── test_server_async.cpp                # Configure ephemeral ports (0) for parallel CTest isolation
    ├── test_graceful_shutdown.cpp           # Configure ephemeral ports (0) + verify network draining
    ├── test_concurrency.cpp                 # Configure ephemeral ports (0) for parallel CTest isolation
    ├── test_validation_integration.cpp      # Configure ephemeral ports (0) for parallel CTest isolation
    └── test_live_ingress.cpp                # Live RPC, OpenAI REST/SSE, /v1/models, /health, /metrics, drain & fault tests

deploy & scripts & CI:
├── CMakeLists.txt                           # Add json.cpp, http_server.cpp, rpc_server.cpp, rpc_client.cpp, new test files
├── .env.example                             # Document HTTP_PORT=8080 alongside GATEWAY_PORT & METRICS_PORT
├── deploy/.env.example                      # Document HTTP_PORT=8080
├── Dockerfile & deploy/Dockerfile           # EXPOSE 50051 8080 9090, ENV HTTP_PORT=8080
├── deploy/docker-compose.yml                # Expose 8080, pass HTTP_PORT, enable mock upstreams when keys empty if needed
├── scripts/verify_metrics_format.sh         # Verify live server GET /metrics cleanly
└── .github/workflows/ci.yml                 # Start gateway_server --mock-redis --mock-upstreams for CLI & HTTP checks
```

**Structure Decision**: Keep Tessera's clean `tessera_core` static library architecture (`CMakeLists.txt`), adding modular `json`, `http_server`, `rpc_server`, and `rpc_client` units inside `tessera_core` so both `gateway_server`, `gateway_client`, and GoogleTest suites share tested networking and OpenAI JSON logic cleanly without external runtime dependencies.

---

## Complexity Tracking

No Constitution violations identified. Dual-transport compilation (`TESSERA_HAS_SYSTEM_GRPC` vs. POSIX TCP RPC fallback) preserves portability cleanly inside `rpc_server.cpp` and `rpc_client.cpp` with identical external contracts.
