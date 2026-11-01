#include "tessera/server/gateway_server.hpp"
#include "tessera/common/logger.hpp"
#include <csignal>
#include <chrono>

namespace tessera {

static GatewayServer* g_server_instance = nullptr;

static void HandleSignal(int sig) {
  if (g_server_instance != nullptr) {
    TESSERA_LOG_INFO("Received signal {}, initiating graceful shutdown...", sig);
    g_server_instance->Stop();
  }
}

GatewayServer::GatewayServer(ServerConfig config, std::shared_ptr<ICompletionBackend> backend)
    : config_(std::move(config)),
      backend_(std::move(backend)),
      health_service_(HealthStatus::SERVING) {}

GatewayServer::~GatewayServer() {
  Stop();
}

bool GatewayServer::Start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    TESSERA_LOG_WARN("GatewayServer is already running on {}", config_.Address());
    return true;
  }

  draining_.store(false, std::memory_order_release);
  health_service_.SetStatus(HealthStatus::SERVING);

  TESSERA_LOG_INFO(
      "Starting Tessera Gateway on {} with {} worker threads (Max Payload: {} MB)...",
      config_.Address(), config_.worker_threads, config_.max_message_size / (1024 * 1024));

  for (uint32_t i = 0; i < config_.worker_threads; ++i) {
    workers_.emplace_back(&GatewayServer::WorkerLoop, this, i);
  }

  TESSERA_LOG_INFO("Gateway server successfully initialized and healthy.");
  return true;
}

void GatewayServer::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  // 1. Mark status as NOT_SERVING so health probes immediately failover/drain
  health_service_.SetStatus(HealthStatus::NOT_SERVING);
  draining_.store(true, std::memory_order_release);
  TESSERA_LOG_INFO("Gateway health set to NOT_SERVING. Draining {} in-flight requests...",
                   in_flight_requests_.load(std::memory_order_acquire));

  // 2. Wait up to graceful_drain_timeout_ms for in-flight requests to complete
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.graceful_drain_timeout_ms);
  {
    std::unique_lock<std::mutex> lock(cv_mutex_);
    drain_cv_.wait_until(lock, deadline, [this]() {
      return in_flight_requests_.load(std::memory_order_acquire) == 0;
    });
  }

  int32_t remaining = in_flight_requests_.load(std::memory_order_acquire);
  if (remaining > 0) {
    TESSERA_LOG_WARN("Drain timeout elapsed with {} requests still in flight. Forcing shutdown.", remaining);
  } else {
    TESSERA_LOG_INFO("All in-flight requests drained cleanly.");
  }

  // 3. Stop worker loops
  running_.store(false, std::memory_order_release);

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();

  TESSERA_LOG_INFO("Gateway server shutdown complete.");
}

void GatewayServer::Wait() {
  while (running_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void GatewayServer::ProcessRequestAsync(
    ChatCompletionRequest request,
    std::function<void(const ChatCompletionResponse&, Status)> on_finish) {
  if (!running_.load(std::memory_order_acquire) || draining_.load(std::memory_order_acquire)) {
    ChatCompletionResponse resp;
    resp.correlation_id = request.correlation_id;
    if (on_finish) {
      on_finish(resp, Status::Unavailable("Gateway server is shutting down or not serving"));
    }
    return;
  }

  in_flight_requests_.fetch_add(1, std::memory_order_acq_rel);

  auto wrapped_finish = [this, user_cb = std::move(on_finish)](
                            const ChatCompletionResponse& resp, Status status) {
    if (user_cb) {
      user_cb(resp, status);
    }
    int32_t current = in_flight_requests_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (current == 0 && draining_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(cv_mutex_);
      drain_cv_.notify_all();
    }
  };

  auto call_data = std::make_unique<ChatCompletionCallData>(
      backend_.get(), std::move(request), std::move(wrapped_finish), config_.max_message_size);

  call_data->Proceed(true);
}

Status GatewayServer::ProcessRequestSync(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  Status result_status = Status::OK();
  bool completed = false;
  std::mutex mtx;
  std::condition_variable cv;

  ProcessRequestAsync(
      request,
      [&](const ChatCompletionResponse& resp, Status status) {
        std::lock_guard<std::mutex> lock(mtx);
        *response = resp;
        result_status = status;
        completed = true;
        cv.notify_one();
      });

  std::unique_lock<std::mutex> lock(mtx);
  cv.wait(lock, [&]() { return completed; });

  return result_status;
}

void GatewayServer::WorkerLoop(uint32_t worker_id) {
  TESSERA_LOG_DEBUG("Worker thread {} started.", worker_id);
  while (running_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  TESSERA_LOG_DEBUG("Worker thread {} exited.", worker_id);
}

void GatewayServer::RegisterSignalHandler(GatewayServer* instance) {
  g_server_instance = instance;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
}

}  // namespace tessera

