#include "tessera/server/gateway_server.hpp"
#include "tessera/common/correlation.hpp"
#include "tessera/common/json.hpp"
#include "tessera/common/logger.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>

namespace tessera {

namespace {

GatewayServer* g_server_instance = nullptr;

void HandleSignal(int sig) {
  if (g_server_instance != nullptr) {
    TESSERA_LOG_INFO("Received signal {}, initiating graceful shutdown...", sig);
    g_server_instance->Stop();
  }
}

std::string ToLowerCopy(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char ch : sv) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string ExtractRetryAfterSeconds(std::string_view msg) {
  auto pos = msg.find("Retry after ");
  if (pos == std::string_view::npos) return "";
  pos += 12;
  uint64_t ms = 0;
  bool any = false;
  while (pos < msg.size() && std::isdigit(static_cast<unsigned char>(msg[pos]))) {
    ms = ms * 10 + static_cast<uint64_t>(msg[pos] - '0');
    any = true;
    ++pos;
  }
  if (!any) return "";
  uint64_t secs = std::max<uint64_t>(1u, (ms + 999u) / 1000u);
  return std::to_string(secs);
}

std::vector<std::string_view> ChunkContentForSse(std::string_view text) {
  std::vector<std::string_view> chunks;
  if (text.empty()) {
    return chunks;
  }
  constexpr size_t kTargetSlice = 16;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t next = std::min(text.size(), pos + kTargetSlice);
    while (next < text.size() && text[next] != ' ' && (next - pos) < 32) {
      ++next;
    }
    if (next < text.size() && text[next] == ' ') {
      ++next;  // Keep trailing space attached cleanly to preserve exact reassembly
    }
    chunks.push_back(text.substr(pos, next - pos));
    pos = next;
  }
  return chunks;
}

class InFlightGuard {
 public:
  explicit InFlightGuard(std::function<void()> on_release)
      : on_release_(std::move(on_release)) {}
  ~InFlightGuard() {
    if (active_ && on_release_) {
      on_release_();
    }
  }
  InFlightGuard(const InFlightGuard&) = delete;
  InFlightGuard& operator=(const InFlightGuard&) = delete;

 private:
  bool active_{true};
  std::function<void()> on_release_;
};

}  // namespace

GatewayServer::GatewayServer(ServerConfig config, std::shared_ptr<ICompletionBackend> backend)
    : config_(std::move(config)),
      backend_(std::move(backend)),
      health_service_(HealthStatus::SERVING) {}

GatewayServer::~GatewayServer() {
  Stop();
}

void GatewayServer::RegisterSignalHandler(GatewayServer* instance) {
  g_server_instance = instance;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGPIPE, SIG_IGN);
}

bool GatewayServer::Start() {
  if (running_.load(std::memory_order_acquire)) {
    TESSERA_LOG_WARN("GatewayServer is already running on {}", config_.Address());
    return true;
  }

  draining_.store(false, std::memory_order_release);
  health_service_.SetStatus(HealthStatus::SERVING);
  unified_http_metrics_ = false;
  running_.store(true, std::memory_order_release);

  TESSERA_LOG_INFO(
      "Starting Tessera Gateway on {} with {} worker threads (Max Payload: {} MB)...",
      config_.Address(), config_.worker_threads, config_.max_message_size / (1024 * 1024));

  if (config_.enable_rpc_listener) {
    rpc_server_ = std::make_unique<RpcServer>(
        config_.host,
        config_.port,
        config_.worker_threads,
        config_.max_message_size,
        [this](ChatCompletionRequest req, RpcServer::CompletionCallback cb) {
          ProcessRequestAsync(std::move(req), std::move(cb));
        },
        [this]() { return CheckHealth(); });
    if (!rpc_server_->Start()) {
      TESSERA_LOG_ERROR("Failed starting RPC listener on {}:{}", config_.host, config_.port);
      rpc_server_.reset();
      running_.store(false, std::memory_order_release);
      health_service_.SetStatus(HealthStatus::NOT_SERVING);
      return false;
    }
  }

  bool share_http_and_metrics =
      config_.enable_http_listener && config_.enable_metrics_listener &&
      config_.http_port > 0 && config_.http_port == config_.metrics_port;

  if (config_.enable_http_listener) {
    http_server_ = std::make_unique<HttpServer>(
        config_.host,
        config_.http_port,
        config_.worker_threads,
        config_.max_message_size,
        [this, share_http_and_metrics](const HttpRequest& req, HttpResponseWriter& writer) {
          HandleHttpRoute(req, writer, true, share_http_and_metrics);
        });
    if (!http_server_->Start()) {
      TESSERA_LOG_ERROR("Failed starting HTTP ingress listener on {}:{}", config_.host, config_.http_port);
      http_server_.reset();
      if (rpc_server_) {
        rpc_server_->Stop();
        rpc_server_.reset();
      }
      running_.store(false, std::memory_order_release);
      health_service_.SetStatus(HealthStatus::NOT_SERVING);
      return false;
    }
    unified_http_metrics_ = share_http_and_metrics;
  }

  if (config_.enable_metrics_listener && !share_http_and_metrics) {
    metrics_server_ = std::make_unique<HttpServer>(
        config_.host,
        config_.metrics_port,
        std::max(1u, std::min(config_.worker_threads, 4u)),
        config_.max_message_size,
        [this](const HttpRequest& req, HttpResponseWriter& writer) {
          HandleHttpRoute(req, writer, false, true);
        });
    if (!metrics_server_->Start()) {
      TESSERA_LOG_ERROR(
          "Failed starting Prometheus metrics HTTP listener on {}:{}",
          config_.host,
          config_.metrics_port);
      metrics_server_.reset();
      if (http_server_) {
        http_server_->Stop();
        http_server_.reset();
      }
      if (rpc_server_) {
        rpc_server_->Stop();
        rpc_server_.reset();
      }
      running_.store(false, std::memory_order_release);
      health_service_.SetStatus(HealthStatus::NOT_SERVING);
      return false;
    }
  }

  TESSERA_LOG_INFO("Gateway server successfully initialized and healthy.");
  return true;
}

void GatewayServer::Stop() {
  if (!running_.load(std::memory_order_acquire) &&
      !rpc_server_ && !http_server_ && !metrics_server_) {
    return;
  }

  // 1. Mark NOT_SERVING immediately so health probes and admission gates reject new work
  health_service_.SetStatus(HealthStatus::NOT_SERVING);
  draining_.store(true, std::memory_order_release);
  TESSERA_LOG_INFO("Gateway health set to NOT_SERVING. Draining {} in-flight requests...",
                   in_flight_requests_.load(std::memory_order_acquire));

  // 2. Wait up to graceful_drain_timeout_ms for all active RPC / HTTP / SSE work
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(config_.graceful_drain_timeout_ms);
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

  // 3. Stop network listeners and wake Wait() callers
  running_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    wait_cv_.notify_all();
  }

  if (metrics_server_) {
    metrics_server_->Stop();
    metrics_server_.reset();
  }
  if (http_server_) {
    http_server_->Stop();
    http_server_.reset();
  }
  if (rpc_server_) {
    rpc_server_->Stop();
    rpc_server_.reset();
  }
  unified_http_metrics_ = false;

  TESSERA_LOG_INFO("Gateway server shutdown complete.");
}

void GatewayServer::Wait() {
  std::unique_lock<std::mutex> lock(wait_mutex_);
  wait_cv_.wait(lock, [this]() { return !running_.load(std::memory_order_acquire); });
}

bool GatewayServer::TryAcquireInFlight() noexcept {
  if (!running_.load(std::memory_order_acquire) || draining_.load(std::memory_order_acquire)) {
    return false;
  }
  in_flight_requests_.fetch_add(1, std::memory_order_acq_rel);
  if (!running_.load(std::memory_order_acquire) || draining_.load(std::memory_order_acquire)) {
    ReleaseInFlight();
    return false;
  }
  return true;
}

void GatewayServer::ReleaseInFlight() noexcept {
  int32_t current = in_flight_requests_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (current == 0 && draining_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    drain_cv_.notify_all();
  }
}

void GatewayServer::ProcessRequestAsync(
    ChatCompletionRequest request,
    std::function<void(const ChatCompletionResponse&, Status)> on_finish) {
  if (!TryAcquireInFlight()) {
    ChatCompletionResponse resp;
    resp.correlation_id = request.correlation_id;
    if (on_finish) {
      on_finish(resp, Status::Unavailable("Gateway server is shutting down or not serving"));
    }
    return;
  }

  auto wrapped_finish = [this, user_cb = std::move(on_finish)](
                            const ChatCompletionResponse& resp, Status status) {
    if (user_cb) {
      user_cb(resp, status);
    }
    ReleaseInFlight();
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

void GatewayServer::SetModelListProvider(std::function<std::vector<ModelInfo>()> provider) {
  std::lock_guard<std::mutex> lock(model_provider_mutex_);
  model_list_provider_ = std::move(provider);
}

std::vector<ModelInfo> GatewayServer::ListModels() const {
  std::function<std::vector<ModelInfo>()> provider;
  {
    std::lock_guard<std::mutex> lock(model_provider_mutex_);
    provider = model_list_provider_;
  }
  if (provider) {
    return provider();
  }
  return {};
}

uint16_t GatewayServer::BoundPort() const noexcept {
  return rpc_server_ ? rpc_server_->BoundPort() : 0;
}

uint16_t GatewayServer::BoundHttpPort() const noexcept {
  return http_server_ ? http_server_->BoundPort() : 0;
}

uint16_t GatewayServer::BoundMetricsPort() const noexcept {
  if (unified_http_metrics_ && http_server_) {
    return http_server_->BoundPort();
  }
  return metrics_server_ ? metrics_server_->BoundPort() : 0;
}

void GatewayServer::SendMappedHttpError(HttpResponseWriter& writer, const Status& status) const {
  if (status.code == StatusCode::INVALID_ARGUMENT) {
    writer.SendResponse(
        400,
        "Bad Request",
        "application/json",
        json_codec::SerializeOpenAIError(status.message, "invalid_request_error", "invalid_argument"));
    return;
  }

  if (status.code == StatusCode::RESOURCE_EXHAUSTED) {
    if (status.message.find("exceeds") != std::string::npos) {
      writer.SendResponse(
          413,
          "Payload Too Large",
          "application/json",
          json_codec::SerializeOpenAIError(status.message, "invalid_request_error", "payload_too_large"));
      return;
    }
    std::vector<std::pair<std::string, std::string>> headers;
    std::string retry_sec = ExtractRetryAfterSeconds(status.message);
    if (!retry_sec.empty()) {
      headers.emplace_back("Retry-After", std::move(retry_sec));
    }
    writer.SendResponse(
        429,
        "Too Many Requests",
        "application/json",
        json_codec::SerializeOpenAIError(status.message, "rate_limit_error", "rate_limit_exceeded"),
        headers);
    return;
  }

  writer.SendResponse(
      503,
      "Service Unavailable",
      "application/json",
      json_codec::SerializeOpenAIError(status.message, "server_error", "service_unavailable"));
}

void GatewayServer::HandleHttpRoute(
    const HttpRequest& req,
    HttpResponseWriter& writer,
    bool allow_openai,
    bool allow_metrics) {
  if (req.path == "/health") {
    if (req.method != "GET") {
      writer.SendResponse(
          405,
          "Method Not Allowed",
          "application/json",
          json_codec::SerializeOpenAIError("GET method required for /health", "invalid_request_error"));
      return;
    }
    bool healthy = CheckHealth() == HealthStatus::SERVING &&
                   running_.load(std::memory_order_acquire) &&
                   !draining_.load(std::memory_order_acquire);
    JsonValue body = JsonValue::MakeObject();
    body.Set("status", healthy ? "SERVING" : "NOT_SERVING");
    writer.SendResponse(
        healthy ? 200 : 503,
        healthy ? "OK" : "Service Unavailable",
        "application/json",
        body.Dump());
    return;
  }

  if (allow_metrics && req.path == "/metrics") {
    if (req.method != "GET") {
      writer.SendResponse(
          405,
          "Method Not Allowed",
          "application/json",
          json_codec::SerializeOpenAIError("GET method required for /metrics", "invalid_request_error"));
      return;
    }
    writer.SendResponse(
        200,
        "OK",
        "text/plain; version=0.0.4; charset=utf-8",
        TelemetryRegistry::Instance().ToPrometheusFormat());
    return;
  }

  if (allow_openai && req.path == "/v1/models") {
    if (req.method != "GET") {
      writer.SendResponse(
          405,
          "Method Not Allowed",
          "application/json",
          json_codec::SerializeOpenAIError("GET method required for /v1/models", "invalid_request_error"));
      return;
    }
    auto models = ListModels();
    std::vector<json_codec::ListedModel> listed;
    listed.reserve(models.size());
    for (const auto& m : models) {
      listed.push_back(json_codec::ListedModel{
          .id = m.id,
          .owned_by = m.owned_by,
          .created = m.created,
      });
    }
    writer.SendResponse(
        200,
        "OK",
        "application/json",
        json_codec::SerializeOpenAIModels(listed));
    return;
  }

  if (allow_openai && req.path == "/v1/chat/completions") {
    if (req.method != "POST") {
      writer.SendResponse(
          405,
          "Method Not Allowed",
          "application/json",
          json_codec::SerializeOpenAIError(
              "POST method required for /v1/chat/completions", "invalid_request_error"));
      return;
    }

    if (!TryAcquireInFlight()) {
      SendMappedHttpError(
          writer, Status::Unavailable("Gateway server is shutting down or not serving"));
      return;
    }
    InFlightGuard guard([this]() { ReleaseInFlight(); });

    if (req.body.size() > config_.max_message_size) {
      SendMappedHttpError(
          writer,
          Status::ResourceExhausted(
              "Request size exceeds configured maximum limit of " +
              std::to_string(config_.max_message_size) + " bytes"));
      return;
    }

    ChatCompletionRequest chat_req;
    Status parse_status = json_codec::ParseOpenAIChatRequest(req.body, &chat_req);
    if (!parse_status.ok()) {
      SendMappedHttpError(writer, parse_status);
      return;
    }

    if (chat_req.correlation_id.empty()) {
      std::string hdr_cid = req.Header("x-correlation-id");
      chat_req.correlation_id = hdr_cid.empty() ? CorrelationId::Generate() : std::move(hdr_cid);
    }
    std::string cache_ctrl = ToLowerCopy(req.Header("cache-control"));
    if (cache_ctrl.find("no-cache") != std::string::npos) {
      chat_req.no_cache = true;
    }

    Status val_status = ValidateRequest(chat_req, config_.max_message_size);
    if (!val_status.ok()) {
      SendMappedHttpError(writer, val_status);
      return;
    }

    ChatCompletionResponse resp;
    Status exec_status = Status::OK();
    if (backend_ != nullptr) {
      exec_status = backend_->Complete(chat_req, &resp);
    } else {
      exec_status = Status::Internal("No backend available to process completion");
    }

    if (!exec_status.ok()) {
      SendMappedHttpError(writer, exec_status);
      return;
    }

    if (resp.id.empty()) {
      resp.id = "chatcmpl-" + chat_req.correlation_id;
    }
    if (resp.created == 0) {
      resp.created = static_cast<int64_t>(std::time(nullptr));
    }
    if (resp.correlation_id.empty()) {
      resp.correlation_id = chat_req.correlation_id;
    }

    std::vector<std::pair<std::string, std::string>> extra_headers = {
        {"X-Correlation-Id", resp.correlation_id},
        {"X-Tessera-Cache-Hit", resp.cache_hit ? "true" : "false"},
    };
    if (!resp.cache_tier.empty()) {
      extra_headers.emplace_back("X-Tessera-Cache-Tier", resp.cache_tier);
    }

    if (!chat_req.stream) {
      writer.SendResponse(
          200,
          "OK",
          "application/json",
          json_codec::SerializeOpenAIChatResponse(resp),
          extra_headers);
      return;
    }

    if (!writer.BeginSseStream(extra_headers)) {
      return;
    }
    if (!writer.WriteSseData(json_codec::SerializeOpenAISseChunk(resp, "assistant", "", ""))) {
      return;
    }

    std::string_view full_content;
    std::string_view finish_reason = "stop";
    if (!resp.choices.empty()) {
      full_content = resp.choices[0].message.content;
      if (!resp.choices[0].finish_reason.empty()) {
        finish_reason = resp.choices[0].finish_reason;
      }
    }

    for (std::string_view slice : ChunkContentForSse(full_content)) {
      if (!writer.WriteSseData(json_codec::SerializeOpenAISseChunk(resp, "", slice, ""))) {
        return;
      }
    }

    if (!writer.WriteSseData(json_codec::SerializeOpenAISseChunk(resp, "", "", finish_reason))) {
      return;
    }
    writer.WriteSseData("[DONE]");
    return;
  }

  writer.SendResponse(
      404,
      "Not Found",
      "application/json",
      json_codec::SerializeOpenAIError("Unknown endpoint path '" + req.path + "'", "invalid_request_error"));
}

}  // namespace tessera
