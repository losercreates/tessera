# Interface & Network Contracts: Live Network Ingress

**Feature**: `010-live-network-ingress`  
**Contract Version**: 1.0.0  
**Scope**: `GatewayServer`, `HttpServer`, `RpcServer` / `RpcClient`, `protos/gateway.proto`, `gateway_client` CLI

---

## 1. Protobuf Schema Extensions (`protos/gateway.proto`)

```protobuf
message ChatCompletionRequest {
  string model = 1;
  repeated ChatMessage messages = 2;
  optional float temperature = 3;
  optional uint32 max_tokens = 4;
  bool stream = 5;
  string correlation_id = 6;
  bool no_cache = 7;
}

message ChatCompletionResponse {
  string id = 1;
  int64 created = 2;
  string model = 3;
  repeated Choice choices = 4;
  Usage usage = 5;
  string correlation_id = 6;
  bool cache_hit = 7;
  string cache_tier = 8;
}
```

---

## 2. `GatewayServer` & Network Ingress Interfaces

```cpp
// include/tessera/server/server_config.hpp
struct ServerConfig {
  std::string host{"0.0.0.0"};
  uint16_t port{50051};
  uint16_t http_port{8080};
  uint16_t metrics_port{9090};
  uint32_t worker_threads{std::max(2u, std::thread::hardware_concurrency())};
  size_t max_message_size{4 * 1024 * 1024};
  uint32_t graceful_drain_timeout_ms{5000};
  bool enable_rpc_listener{true};
  bool enable_http_listener{true};
  bool enable_metrics_listener{true};
};

// include/tessera/server/gateway_server.hpp
struct ModelInfo {
  std::string id;
  std::string owned_by;
  int64_t created{0};
};

class GatewayServer {
 public:
  ...
  void SetModelListProvider(std::function<std::vector<ModelInfo>()> provider);
  [[nodiscard]] std::vector<ModelInfo> ListModels() const; // returns empty vector when provider callback is unset

  [[nodiscard]] uint16_t BoundPort() const noexcept;
  [[nodiscard]] uint16_t BoundHttpPort() const noexcept;
  [[nodiscard]] uint16_t BoundMetricsPort() const noexcept;
  ...
};
```

### Invariants
1. `Start()` binds configured RPC (`port`), HTTP (`http_port`), and Prometheus (`metrics_port`) listeners synchronously before returning `true`; if any enabled bind fails, all previously started listeners are torn down cleanly and `Start()` returns `false`.
2. `Stop()` sets `HealthStatus::NOT_SERVING` immediately, unblocks new admission with `503`/`UNAVAILABLE`, waits up to `graceful_drain_timeout_ms` for `in_flight_requests_ == 0`, closes sockets/gRPC queues, and joins all listener and worker threads cleanly (idempotent on repeated `Stop()` / destructor calls).
3. `BoundPort()`, `BoundHttpPort()`, and `BoundMetricsPort()` return the active host-byte-order TCP port numbers immediately after `Start()` returns `true` (enabling `port = 0` ephemeral testing).

---

## 3. OpenAI HTTP/REST/SSE & Prometheus Endpoint Contracts

### `HTTP_PORT` (default `8080`)
- **`POST /v1/chat/completions`**:
  - Accepts `application/json` OpenAI body (`model`, `messages`, optional `temperature`, `max_tokens`, `stream`, `no_cache`, `correlation_id`) plus headers `X-Correlation-Id` and `Cache-Control: no-cache`.
  - Non-streaming (`"stream": false` or omitted): returns `200 OK` (`application/json`) OpenAI `chat.completion` object + `X-Correlation-Id` and `X-Tessera-Cache-Hit: true|false` (`X-Tessera-Cache-Tier` on hit).
  - Streaming (`"stream": true`): returns `200 OK` (`Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: close`) emitting progressive `data: {"id":...,"object":"chat.completion.chunk",...}\n\n` events ending with finish chunk and `data: [DONE]\n\n`.
  - Error responses (`400`, `413`, `429`, `503`): return OpenAI JSON `{"error":{"message":"...","type":"...","code":"..."}}` (`429` includes `Retry-After` seconds header when retry window is present in error message).
- **`GET /v1/models`**:
  - Returns `200 OK` (`application/json`): `{"object":"list","data":[{"id":"...","object":"model","created":...,"owned_by":"..."}]}`.
- **`GET /health`**:
  - Returns `200 OK` `{"status":"SERVING"}` when healthy; `503 Service Unavailable` `{"status":"NOT_SERVING"}` during drain/unhealthy state.

### `METRICS_PORT` (default `9090`)
- **`GET /metrics`**:
  - Returns `200 OK` (`Content-Type: text/plain; version=0.0.4; charset=utf-8`) body equal to `TelemetryRegistry::Instance().ToPrometheusFormat()`.
- **`GET /health`**:
  - Same readiness JSON & status code as ingress `/health`.

---

## 4. Remote `gateway_client` CLI Contract

- **Target Resolution**:
  - Defaults to `127.0.0.1` (`GATEWAY_HOST`), RPC port `50051` (`GATEWAY_PORT`), HTTP port `8080` (`HTTP_PORT`), Metrics port `9090` (`METRICS_PORT`).
  - CLI flags: `--host=<addr>`, `--port=<u16>`, `--http-port=<u16>`, `--metrics-port=<u16>`, `--health`, `--metrics`, `--message=<text>` (or `--message <text>`), `--model=<name>`, `--no-cache`, `--http`, `--stream`, `--timeout-ms=<ms>`, `--help`.
- **Execution Modes**:
  - `--health`: Calls remote health check (`RpcClient::CheckHealth()`, or HTTP `GET /health` with `--http`); exits `0` iff `SERVING`, `1` otherwise.
  - Completion request (`--message` provided, or default completion if `--health` and `--metrics` are not standalone; defaults `--model` to `"auto"` when omitted): dials live server via `RpcClient::Complete()` (or HTTP/SSE when `--http` / `--stream` is specified), prints response & exit `0` on success or stderr error & exit `1` on connection/RPC/HTTP failure.
  - `--metrics`: Fetches live Prometheus payload from `http://<host>:<metrics_port>/metrics` (after sending `--message` if supplied, or standalone if `--message` omitted) and prints exposition text to stdout.
