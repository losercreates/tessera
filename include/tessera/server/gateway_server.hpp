#pragma once

#include "tessera/server/server_config.hpp"
#include "tessera/server/health_service.hpp"
#include "tessera/server/call_data.hpp"
#include "tessera/backend/completion_backend.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <condition_variable>

namespace tessera {

class GatewayServer {
 public:
  GatewayServer(ServerConfig config, std::shared_ptr<ICompletionBackend> backend);
  ~GatewayServer();

  // Start server and worker thread pool
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

  [[nodiscard]] const ServerConfig& Config() const noexcept { return config_; }

  // Signal handler setup
  static void RegisterSignalHandler(GatewayServer* instance);

 private:
  void WorkerLoop(uint32_t worker_id);

  ServerConfig config_;
  std::shared_ptr<ICompletionBackend> backend_;
  HealthService health_service_;

  std::atomic<bool> running_{false};
  std::atomic<bool> draining_{false};
  std::atomic<int32_t> in_flight_requests_{0};

  std::vector<std::thread> workers_;
  std::mutex cv_mutex_;
  std::condition_variable drain_cv_;
};

}  // namespace tessera

