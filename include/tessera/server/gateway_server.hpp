#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/server/call_data.hpp"
#include "tessera/server/health_service.hpp"
#include "tessera/server/http_server.hpp"
#include "tessera/server/rpc_server.hpp"
#include "tessera/server/server_config.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tessera {

struct ModelInfo {
  std::string id;
  std::string owned_by;
  int64_t created{0};
};

class GatewayServer {
 public:
  GatewayServer(ServerConfig config, std::shared_ptr<ICompletionBackend> backend);
  ~GatewayServer();

  GatewayServer(const GatewayServer&) = delete;
  GatewayServer& operator=(const GatewayServer&) = delete;

  // Bind configured RPC/HTTP/Metrics network listeners and start serving
  bool Start();

  // Initiate graceful shutdown with connection draining
  void Stop();

  // Blocking wait until server stops
  void Wait();

  // Asynchronous invocation endpoint
  void ProcessRequestAsync(
      ChatCompletionRequest request,
      std::function<void(const ChatCompletionResponse&, Status)> on_finish);

  // Synchronous invocation helper
  Status ProcessRequestSync(
      const ChatCompletionRequest& request,
      ChatCompletionResponse* response);

  // Model discovery hook for GET /v1/models
  void SetModelListProvider(std::function<std::vector<ModelInfo>()> provider);
  [[nodiscard]] std::vector<ModelInfo> ListModels() const;

  // Health probe
  [[nodiscard]] HealthStatus CheckHealth() const noexcept {
    return health_service_.GetStatus();
  }

  // Active in-flight request count
  [[nodiscard]] int32_t InFlightCount() const noexcept {
    return in_flight_requests_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] uint16_t BoundPort() const noexcept;
  [[nodiscard]] uint16_t BoundHttpPort() const noexcept;
  [[nodiscard]] uint16_t BoundMetricsPort() const noexcept;

  [[nodiscard]] const ServerConfig& Config() const noexcept { return config_; }

  // Signal handler setup
  static void RegisterSignalHandler(GatewayServer* instance);

 private:
  bool TryAcquireInFlight() noexcept;
  void ReleaseInFlight() noexcept;
  void HandleHttpRoute(
      const HttpRequest& req,
      HttpResponseWriter& writer,
      bool allow_openai,
      bool allow_metrics);
  void SendMappedHttpError(HttpResponseWriter& writer, const Status& status) const;

  ServerConfig config_;
  std::shared_ptr<ICompletionBackend> backend_;
  HealthService health_service_;

  mutable std::mutex model_provider_mutex_;
  std::function<std::vector<ModelInfo>()> model_list_provider_;

  std::unique_ptr<RpcServer> rpc_server_;
  std::unique_ptr<HttpServer> http_server_;
  std::unique_ptr<HttpServer> metrics_server_;
  bool unified_http_metrics_{false};

  std::atomic<bool> running_{false};
  std::atomic<bool> draining_{false};
  std::atomic<int32_t> in_flight_requests_{0};

  std::mutex cv_mutex_;
  std::condition_variable drain_cv_;
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
};

}  // namespace tessera
