#include "tessera/routing/cascade_router.hpp"
#include "tessera/common/logger.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"

namespace tessera {

CascadeRouter::CascadeRouter(std::vector<CascadeTarget> targets, std::string route_id)
    : targets_(std::move(targets)), route_id_(std::move(route_id)) {}

Status CascadeRouter::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_requests++;
  }

  if (targets_.empty()) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.failed_requests++;
    stats_.all_providers_failed++;
    return Status::Unavailable("No targets configured in cascade route '" + route_id_ + "'");
  }

  std::string last_error = "Unknown error";
  int last_status = 503;

  for (size_t i = 0; i < targets_.size(); ++i) {
    const auto& target = targets_[i];
    if (target.client == nullptr) {
      continue;
    }

    std::string prov_name = std::string(target.client->ProviderName());

    // Circuit Breaker admission check
    if (target.breaker != nullptr && !target.breaker->CanAttempt()) {
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.direct_bypasses++;
      }
      TESSERA_LOG_INFO("Candidate {} ({}) circuit is OPEN. Directly bypassing to next tier for correlation {}",
                       i, prov_name, request.correlation_id);
      continue;
    }

    if (i > 0) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.failover_attempts++;
    }

    // Build upstream request
    UpstreamRequest req;
    req.provider = prov_name;
    if (!target.target_model.empty()) {
      req.model = target.target_model;
    } else if (!request.model.empty()) {
      req.model = request.model;
    } else {
      req.model = std::string(target.client->DefaultModel());
    }
    req.messages = request.messages;
    req.temperature = request.temperature.value_or(0.7f);
    req.max_tokens = request.max_tokens.value_or(1024);
    req.correlation_id = request.correlation_id;

    // Execute upstream client
    UpstreamResponse resp = target.client->Execute(req);

    // Dynamic Telemetry attribution
    TelemetryRegistry::Instance().RecordUpstreamLatency(prov_name, resp.latency_ms);
    TelemetryRegistry::Instance().IncrementRequestsTotal(req.model, prov_name, std::to_string(resp.status_code));

    if (resp.IsSuccess()) {
      if (target.breaker != nullptr) {
        target.breaker->RecordSuccess();
        TelemetryRegistry::Instance().SetCircuitBreakerState(prov_name, target.breaker->GetState());
      }

      // Populate response
      response->id = "cmpl-" + request.correlation_id;
      response->correlation_id = request.correlation_id;
      response->model = resp.model.empty() ? req.model : resp.model;
      response->choices.clear();
      response->choices.push_back(Choice{
          .index = 0,
          .message = ChatMessage{.role = "assistant", .content = resp.completion_text},
          .finish_reason = "stop",
      });
      response->usage.prompt_tokens = resp.prompt_tokens;
      response->usage.completion_tokens = resp.completion_tokens;
      response->usage.total_tokens = resp.total_tokens;

      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.successful_requests++;
      }
      return Status::OK();
    }

    // Upstream failed (429, 5xx, or network failure)
    last_status = resp.status_code;
    last_error = resp.error_message.empty() ? "HTTP status " + std::to_string(resp.status_code) : resp.error_message;

    if (target.breaker != nullptr) {
      target.breaker->RecordFailure(resp.status_code, last_error);
      TelemetryRegistry::Instance().SetCircuitBreakerState(prov_name, target.breaker->GetState());
    }

    TESSERA_LOG_WARN("Upstream {} failed with status {}: {}. Triggering failover to next candidate in cascade...",
                     prov_name, resp.status_code, last_error);
  }

  // All targets in cascade exhausted
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.failed_requests++;
    stats_.all_providers_failed++;
  }

  TESSERA_LOG_ERROR("All {} upstream providers in cascade '{}' failed. Last status: {}, error: {}",
                    targets_.size(), route_id_, last_status, last_error);
  return Status::Unavailable("All upstream providers in cascade exhausted: " + last_error);
}

CascadeRouterStats CascadeRouter::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

void CascadeRouter::ResetStats() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  stats_ = CascadeRouterStats{};
}

}  // namespace tessera
