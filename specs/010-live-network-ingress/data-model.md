# Data Model: Live Network Ingress (gRPC, OpenAI REST/SSE & Prometheus Metrics)

**Feature**: `010-live-network-ingress`  
**Date**: 2026-11-27

---

## 1. Entities & Configuration Model

### Entity 1: `ServerConfig` (Extended Network Listener Configuration)

| Field | Type | Default / Env Var | Semantics |
|---|---|---|---|
| `host` | `std::string` | `"0.0.0.0"` (`GATEWAY_HOST` bind override) | Bind IPv4 address for all active listeners |
| `port` | `uint16_t` | `50051` (`GATEWAY_PORT`) | Primary gRPC / fallback TCP RPC port (`0` = ephemeral OS-assigned) |
| `http_port` | `uint16_t` | `8080` (`HTTP_PORT`) | OpenAI REST/SSE (`/v1/chat/completions`, `/v1/models`) + `/health` port (`0` = ephemeral) |
| `metrics_port` | `uint16_t` | `9090` (`METRICS_PORT`) | Dedicated Prometheus `/metrics` + `/health` HTTP port (`0` = ephemeral) |
| `worker_threads` | `uint32_t` | `std::max(2u, hw_concurrency)` (`WORKER_THREADS`) | Network accept/CQ/request handler thread pool count |
| `max_message_size` | `size_t` | `4 * 1024 * 1024` (4 MB) | Maximum inbound HTTP body / RPC frame / request byte limit |
| `graceful_drain_timeout_ms` | `uint32_t` | `5000` (5s) | Shutdown drain deadline waiting for in-flight RPC & HTTP/SSE requests |
| `enable_rpc_listener` | `bool` | `true` | Enables RPC listener on `port` during `Start()` |
| `enable_http_listener` | `bool` | `true` | Enables OpenAI REST/SSE HTTP listener on `http_port` during `Start()` |
| `enable_metrics_listener` | `bool` | `true` | Enables Prometheus `/metrics` HTTP listener on `metrics_port` during `Start()` |

Runtime query helpers on `GatewayServer`:
- `BoundPort() const noexcept -> uint16_t`
- `BoundHttpPort() const noexcept -> uint16_t`
- `BoundMetricsPort() const noexcept -> uint16_t`
- `SetModelListProvider(std::function<std::vector<ModelInfo>()>)`

---

### Entity 2: `ModelInfo` & OpenAI HTTP / SSE Payloads

- **`ModelInfo`**:
  - `id`: canonical model identifier (`std::string`, e.g., `"llama-3.1-8b-instant"`, `"ollama/llama3.2"`)
  - `owned_by`: provider or `"tessera-gateway"` (`std::string`)
  - `created`: Unix epoch timestamp (`int64_t`)
- **OpenAI `POST /v1/chat/completions` JSON Request Mapping**:
  - `model` (required non-empty string) -> `ChatCompletionRequest::model`
  - `messages` (required non-empty array of `{"role": "...", "content": "..."}`) -> `ChatCompletionRequest::messages`
  - `temperature` (optional float `[0.0, 2.0]`) -> `ChatCompletionRequest::temperature`
  - `max_tokens` (optional positive int) -> `ChatCompletionRequest::max_tokens`
  - `stream` (optional bool, default `false`) -> `ChatCompletionRequest::stream`
  - `no_cache` (optional bool OR HTTP header `Cache-Control: no-cache`) -> `ChatCompletionRequest::no_cache`
  - `correlation_id` (optional string OR HTTP header `X-Correlation-Id`) -> auto-generated UUID if absent
- **OpenAI JSON Completion (`"stream": false`)**:
  - `id`, `object`: `"chat.completion"`, `created`, `model`, `choices`: `[{"index": 0, "message": {"role": "assistant", "content": "..."}, "finish_reason": "stop"}]`, `usage`: `{"prompt_tokens": N, "completion_tokens": M, "total_tokens": N+M}`
- **OpenAI SSE Chunk (`"stream": true`, `Content-Type: text/event-stream`)**:
  - Sequence: initial role delta chunk (`delta: {"role": "assistant", "content": ""}`) -> $1..K$ content slice chunks (`delta: {"content": "<slice>"}`) -> stop chunk (`delta: {}`, `finish_reason: "stop"`) -> literal `data: [DONE]\n\n`
- **HTTP Status & Error Mapping**:
  - `StatusCode::OK` -> `200 OK`
  - Malformed JSON / `StatusCode::INVALID_ARGUMENT` -> `400 Bad Request` (`invalid_request_error`)
  - Oversized payload / `Content-Length > max_message_size` -> `413 Payload Too Large` (`invalid_request_error`)
  - Local rate-limit exhaustion (`StatusCode::RESOURCE_EXHAUSTED` with rate-limit message) -> `429 Too Many Requests` (`rate_limit_error`)
  - Draining / upstream unavailable (`StatusCode::UNAVAILABLE` / `INTERNAL`) -> `503 Service Unavailable` (`server_error`)

---

### Entity 3: Fallback TCP RPC Frame Format (When `TESSERA_HAS_SYSTEM_GRPC` is Unset)

```text
+-----------------------------------+------------------------------------------+
| uint32_t payload_len (big-endian) | UTF-8 JSON Payload (payload_len bytes)   |
+-----------------------------------+------------------------------------------+
  max: max_message_size + 65536
```

- Request JSON:
  - `{"method": "Check", "service": "..."}`
  - `{"method": "ChatCompletion", "request": {model, messages, temperature, max_tokens, stream, correlation_id, no_cache}}`
- Response JSON:
  - `Check`: `{"status": "SERVING" | "NOT_SERVING" | "UNKNOWN"}`
  - `ChatCompletion`: `{"status_code": <int>, "status_message": "...", "response": {id, created, model, choices, usage, correlation_id, cache_hit, cache_tier}}`

---

### Entity 4: Listener Lifecycle & Graceful Drain State Machine

```text
STOPPED ──Start()──► Bind RPC (50051/0) + HTTP (8080/0) + Metrics (9090/0)
                       │  (any bind failure ──► rollback/close bound sockets, return false)
                       ▼
                    SERVING (HealthStatus::SERVING, 200 on /health)
                       │
                    Stop()
                       ▼
                    DRAINING (HealthStatus::NOT_SERVING, 503 on /health, rejecting new work)
                       │  wait until in_flight_requests_ == 0 OR graceful_drain_timeout_ms elapsed
                       ▼
                    STOPPED (signal shutdown pipes + close listen FDs + Shutdown gRPC CQs + join all threads)
```

---

## 2. Validation Rules Summary

1. **Http Payload Bounds Rule**: Reject HTTP requests whose `Content-Length` or received body exceeds `max_message_size` immediately with `413 Payload Too Large` prior to buffering unbounded memory.
2. **Valid OpenAI Schema Rule**: Reject malformed JSON, missing/non-string `model`, empty/non-array `messages`, invalid roles, out-of-range `temperature`, or empty message content cleanly with `400 Bad Request`.
3. **Cache-Bypass Header Parity**: Treat either `"no_cache": true` JSON property or `Cache-Control: no-cache` request header identically (`req.no_cache = true`).
4. **Remote Client Integrity Rule**: `gateway_client` MUST NOT construct an embedded `GatewayServer`; every invocation MUST connect over TCP/gRPC/HTTP to the target server host/ports and propagate server/connection failures cleanly via non-zero exit codes.
