#pragma once

#include "tessera/common/types.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tessera {

/**
 * Network RPC ingress listener bound to GATEWAY_PORT.
 * Uses native asynchronous gRPC ServerCompletionQueue when TESSERA_HAS_SYSTEM_GRPC
 * is enabled, and length-prefixed UTF-8 JSON over POSIX TCP sockets otherwise.
 */
class RpcServer {
 public:
  using CompletionCallback = std::function<void(const ChatCompletionResponse&, Status)>;
  using CompletionDispatchFn = std::function<void(ChatCompletionRequest, CompletionCallback)>;
  using HealthQueryFn = std::function<HealthStatus()>;

  RpcServer(
      std::string host,
      uint16_t port,
      uint32_t worker_threads,
      size_t max_message_size,
      CompletionDispatchFn on_completion,
      HealthQueryFn on_health);
  ~RpcServer();

  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;

  bool Start();
  void Stop();

  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] uint16_t BoundPort() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tessera
