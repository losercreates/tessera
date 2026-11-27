#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <algorithm>

namespace tessera {

struct ServerConfig {
  std::string host{"0.0.0.0"};
  uint16_t port{50051};
  uint16_t http_port{8080};
  uint16_t metrics_port{9090};
  uint32_t worker_threads{std::max(2u, std::thread::hardware_concurrency())};
  size_t max_message_size{4 * 1024 * 1024};  // 4MB
  uint32_t graceful_drain_timeout_ms{5000};  // 5 seconds
  bool enable_rpc_listener{true};
  bool enable_http_listener{true};
  bool enable_metrics_listener{true};

  [[nodiscard]] std::string Address() const {
    return host + ":" + std::to_string(port);
  }
};

}  // namespace tessera

