#include "tessera/routing/cascade_router.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <algorithm>
#include <limits>

namespace tessera {

CascadeRouter::CascadeRouter(
    std::vector<CascadeTarget> targets,
    std::string route_id,
    std::shared_ptr<DistributedRateLimiter> rate_limiter)
    : targets_(std::move(targets)),
      route_id_(std::move(route_id)),
      rate_limiter_(std::move(rate_limiter)) {}

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
  bool saw_local_rate_limit = false;
  bool saw_upstream_failure_or_open = false;
  bool progressed_past_rejection = false;
  uint64_t min_retry_after_ms = std::numeric_limits<uint64_t>::max();

  for (size_t i = 0; i < targets_.size(); ++i) {
    const auto& target = targets_[i];
    if (target.client == nullptr) {
      saw_upstream_failure_or_open = true;
      continue;
    }

    std::string prov_name = std::string(target.client->ProviderName());

    // 1. Circuit Breaker admission check (must run before quota check so OPEN circuits deduct zero tokens)
    if (target.breaker != nullptr && !target.breaker->CanAttempt()) {
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.direct_bypasses++;
      }
      saw_upstream_failure_or_open = true;
      TESSERA_LOG_INFO("Candidate {} ({}) circuit is OPEN. Directly bypassing to next tier for correlation {}",
                       i, prov_name, request.correlation_id);
      continue;
    }

    // 2. Per-candidate distributed rate limit admission check
    std::shared_ptr<DistributedRateLimiter> active_limiter =
        target.rate_limiter ? target.rate_limiter : rate_limiter_;
    if (active_limiter != nullptr) {
      RateLimitResult rl = active_limiter->CheckAndConsume(prov_name, 1.0);
      if (!rl.allowed) {
        if (target.breaker != nullptr) {
          target.breaker->CancelProbe();
        }
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          stats_.rate_limit_skips++;
        }
        TelemetryRegistry::Instance().IncrementRateLimitSkipsTotal(prov_name);
        saw_local_rate_limit = true;
        min_retry_after_ms = std::min(min_retry_after_ms, rl.retry_after_ms);
        progressed_past_rejection = true;
        TESSERA_LOG_WARN(
            "Candidate {} ({}) local rate limit exhausted (retry after {}ms). Failing over for correlation {}",
            i, prov_name, rl.retry_after_ms, request.correlation_id);
        continue;
      }
    }

    // Only record a failover transition when advancing past an upstream failure or local quota skip
    if (progressed_past_rejection) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.failover_attempts++;
    }

    // 3. Build upstream request
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

    // 4. Execute upstream client
    UpstreamResponse resp = target.client->Execute(req);

    // Dynamic Telemetry attribution (only on actual upstream dispatches)
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
    saw_upstream_failure_or_open = true;
    progressed_past_rejection = true;
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

  if (saw_local_rate_limit && !saw_upstream_failure_or_open) {
    uint64_t retry_ms =
        (min_retry_after_ms == std::numeric_limits<uint64_t>::max()) ? 0 : min_retry_after_ms;
    TESSERA_LOG_ERROR(
        "Rate limit exceeded across all {} upstream providers in cascade '{}'. Earliest retry after {}ms",
        targets_.size(), route_id_, retry_ms);
    return Status::ResourceExhausted(
        "Rate limit exceeded across all providers in route '" + route_id_ +
        "'. Retry after " + std::to_string(retry_ms) + "ms");
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
