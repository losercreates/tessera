#include "tessera/routing/failover_router.hpp"
#include "tessera/common/logger.hpp"
#include <chrono>
#include <utility>

namespace tessera {

FailoverRouter::FailoverRouter(
    std::shared_ptr<IUpstreamClient> primary_client,
    std::shared_ptr<ICircuitBreaker> primary_breaker,
    std::shared_ptr<IUpstreamClient> fallback_client,
    std::shared_ptr<ICircuitBreaker> fallback_breaker,
    std::string fallback_model)
    : primary_client_(std::move(primary_client)),
      primary_breaker_(std::move(primary_breaker)),
      fallback_client_(std::move(fallback_client)),
      fallback_breaker_(std::move(fallback_breaker)),
      fallback_model_(std::move(fallback_model)) {}

Status FailoverRouter::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (!response) {
    return Status::InvalidArgument("Response pointer must not be null");
  }

  total_requests_.fetch_add(1, std::memory_order_relaxed);

  // 1. Evaluate primary circuit breaker
  if (primary_breaker_->CanAttempt()) {
    primary_attempts_.fetch_add(1, std::memory_order_relaxed);

    std::string model = request.model.empty()
                            ? std::string(primary_client_->DefaultModel())
                            : request.model;
    UpstreamRequest primary_req = BuildUpstreamRequest(request, primary_client_->ProviderName(), model);

    UpstreamResponse primary_resp = primary_client_->Execute(primary_req);

    if (primary_resp.IsSuccess()) {
      primary_successes_.fetch_add(1, std::memory_order_relaxed);
      primary_breaker_->RecordSuccess();
      PopulateResponse(primary_resp, request, response);
      return Status::OK();
    }

    // Primary failed (429, 5xx, timeout, etc.)
    primary_failures_.fetch_add(1, std::memory_order_relaxed);
    primary_breaker_->RecordFailure(primary_resp.status_code, primary_resp.error_message);

    TESSERA_LOG_WARN(
        "Primary upstream ({}) failed with status {}: {}. Triggering transparent failover to fallback ({}) for correlation {}",
        primary_client_->ProviderName(), primary_resp.status_code, primary_resp.error_message,
        fallback_model_, request.correlation_id);
    failover_attempts_.fetch_add(1, std::memory_order_relaxed);
  } else {
    // Primary is OPEN or in HALF-OPEN concurrent probation
    direct_fallback_routes_.fetch_add(1, std::memory_order_relaxed);
    TESSERA_LOG_INFO(
        "Primary circuit is non-serving (state={}). Directly routing request to fallback ({}) for correlation {}",
        ToString(primary_breaker_->GetState()), fallback_model_, request.correlation_id);
  }

  // 2. Fallback routing to secondary provider (OpenRouter)
  if (!fallback_breaker_->CanAttempt()) {
    all_providers_failed_.fetch_add(1, std::memory_order_relaxed);
    TESSERA_LOG_ERROR(
        "All upstream providers unavailable (Primary and Fallback circuit breakers OPEN) for correlation {}",
        request.correlation_id);
    return Status::Unavailable("All upstream providers are exhausted or unavailable");
  }

  // Remap model name to fallback model while preserving prompt payload
  UpstreamRequest fallback_req = BuildUpstreamRequest(request, fallback_client_->ProviderName(), fallback_model_);
  UpstreamResponse fallback_resp = fallback_client_->Execute(fallback_req);

  if (fallback_resp.IsSuccess()) {
    fallback_successes_.fetch_add(1, std::memory_order_relaxed);
    fallback_breaker_->RecordSuccess();
    PopulateResponse(fallback_resp, request, response);
    return Status::OK();
  }

  fallback_failures_.fetch_add(1, std::memory_order_relaxed);
  fallback_breaker_->RecordFailure(fallback_resp.status_code, fallback_resp.error_message);
  all_providers_failed_.fetch_add(1, std::memory_order_relaxed);

  TESSERA_LOG_ERROR(
      "Both primary and fallback providers failed. Fallback ({}) returned status {}: {} for correlation {}",
      fallback_client_->ProviderName(), fallback_resp.status_code, fallback_resp.error_message,
      request.correlation_id);

  return Status::Unavailable("All upstream providers failed to complete request: " + fallback_resp.error_message);
}

UpstreamRequest FailoverRouter::BuildUpstreamRequest(
    const ChatCompletionRequest& req,
    std::string_view provider,
    std::string_view model) const {
  UpstreamRequest out;
  out.provider = std::string(provider);
  out.model = std::string(model);
  out.messages = req.messages;
  out.temperature = req.temperature.value_or(0.7f);
  out.max_tokens = req.max_tokens.value_or(1024);
  out.correlation_id = req.correlation_id;
  return out;
}

void FailoverRouter::PopulateResponse(
    const UpstreamResponse& upstream_resp,
    const ChatCompletionRequest& orig_req,
    ChatCompletionResponse* response) const {
  response->id = "cmpl-" + orig_req.correlation_id;
  response->created = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  response->model = upstream_resp.model;
  response->correlation_id = orig_req.correlation_id;

  Choice choice;
  choice.index = 0;
  choice.message.role = "assistant";
  choice.message.content = upstream_resp.completion_text;
  choice.finish_reason = "stop";

  response->choices.clear();
  response->choices.push_back(std::move(choice));

  response->usage.prompt_tokens = upstream_resp.prompt_tokens;
  response->usage.completion_tokens = upstream_resp.completion_tokens;
  response->usage.total_tokens = upstream_resp.total_tokens;
  response->cache_hit = false;
  response->cache_tier = "";
}

FailoverRouterStats FailoverRouter::GetStats() const noexcept {
  FailoverRouterStats stats;
  stats.total_requests = total_requests_.load(std::memory_order_relaxed);
  stats.primary_attempts = primary_attempts_.load(std::memory_order_relaxed);
  stats.primary_successes = primary_successes_.load(std::memory_order_relaxed);
  stats.primary_failures = primary_failures_.load(std::memory_order_relaxed);
  stats.failover_attempts = failover_attempts_.load(std::memory_order_relaxed);
  stats.direct_fallback_routes = direct_fallback_routes_.load(std::memory_order_relaxed);
  stats.fallback_successes = fallback_successes_.load(std::memory_order_relaxed);
  stats.fallback_failures = fallback_failures_.load(std::memory_order_relaxed);
  stats.all_providers_failed = all_providers_failed_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace tessera
