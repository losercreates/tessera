#pragma once

#include "tessera/common/types.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera {

/**
 * Remote RPC client dialing a live GatewayServer on GATEWAY_PORT.
 * Uses native gRPC stub when TESSERA_HAS_SYSTEM_GRPC is defined, and
 * framed JSON over TCP otherwise.
 */
class RpcClient {
 public:
  RpcClient(std::string host, uint16_t port, uint32_t timeout_ms = 5000);

  Status Complete(const ChatCompletionRequest& req, ChatCompletionResponse* resp);
  Status CheckHealth(HealthStatus* out_status);

 private:
  std::string host_;
  uint16_t port_;
  uint32_t timeout_ms_;
};

struct HttpClientResponse {
  int status_code{0};
  std::unordered_map<std::string, std::string> headers;  // lowercased header names
  std::string body;

  [[nodiscard]] std::string Header(std::string_view key) const;
};

/**
 * Lightweight synchronous HTTP/1.1 client supporting REST JSON, Prometheus /metrics,
 * /health probes, and progressive text/event-stream SSE event callbacks.
 */
Status HttpRequestSync(
    std::string_view host,
    uint16_t port,
    std::string_view method,
    std::string_view path,
    std::string_view body = "",
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    HttpClientResponse* out_resp = nullptr,
    uint32_t timeout_ms = 5000,
    const std::function<void(std::string_view)>& on_sse_event = {});

}  // namespace tessera
