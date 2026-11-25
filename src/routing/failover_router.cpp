#include "tessera/routing/failover_router.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

FailoverRouter::FailoverRouter(
    std::shared_ptr<IUpstreamClient> primary_client,
    std::shared_ptr<ICircuitBreaker> primary_breaker,
    std::shared_ptr<IUpstreamClient> fallback_client,
    std::shared_ptr<ICircuitBreaker> fallback_breaker,
    std::string fallback_model,
    std::shared_ptr<DistributedRateLimiter> rate_limiter)
    : primary_client_(std::move(primary_client)),
      primary_breaker_(std::move(primary_breaker)),
      fallback_client_(std::move(fallback_client)),
      fallback_breaker_(std::move(fallback_breaker)),
      fallback_model_(std::move(fallback_model)) {
  std::vector<CascadeTarget> targets = {
      CascadeTarget{.client = primary_client_, .breaker = primary_breaker_, .target_model = "", .priority = 0},
      CascadeTarget{.client = fallback_client_, .breaker = fallback_breaker_, .target_model = fallback_model_, .priority = 1},
  };
  cascade_router_ = std::make_unique<CascadeRouter>(
      std::move(targets), "failover_adapter", std::move(rate_limiter));
}

void FailoverRouter::SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter) {
  if (cascade_router_) {
    cascade_router_->SetRateLimiter(std::move(rate_limiter));
  }
}

Status FailoverRouter::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (!response) {
    return Status::InvalidArgument("Response pointer must not be null");
  }

  total_requests_.fetch_add(1, std::memory_order_relaxed);

  auto stats_before = cascade_router_->GetStats();
  Status status = cascade_router_->Complete(request, response);
  auto stats_after = cascade_router_->GetStats();

  uint64_t new_bypasses = stats_after.direct_bypasses - stats_before.direct_bypasses;
  uint64_t new_failovers = stats_after.failover_attempts - stats_before.failover_attempts;
  uint64_t new_rate_skips = stats_after.rate_limit_skips - stats_before.rate_limit_skips;

  if (new_rate_skips > 0) {
    rate_limit_skips_.fetch_add(new_rate_skips, std::memory_order_relaxed);
  }

  if (new_bypasses > 0) {
    direct_fallback_routes_.fetch_add(new_bypasses, std::memory_order_relaxed);
    if (status.ok()) {
      fallback_successes_.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (new_rate_skips > 0) {
    if (new_failovers > 0) {
      failover_attempts_.fetch_add(new_failovers, std::memory_order_relaxed);
    }
    if (status.ok()) {
      fallback_successes_.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    primary_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (new_failovers > 0) {
      failover_attempts_.fetch_add(new_failovers, std::memory_order_relaxed);
      primary_failures_.fetch_add(1, std::memory_order_relaxed);
      if (status.ok()) {
        fallback_successes_.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (status.ok()) {
      primary_successes_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (!status.ok()) {
    fallback_failures_.fetch_add(1, std::memory_order_relaxed);
    all_providers_failed_.fetch_add(1, std::memory_order_relaxed);
    if (status.code == StatusCode::RESOURCE_EXHAUSTED) {
      return status;
    }
    return Status::Unavailable("All upstream providers are exhausted or unavailable");
  }

  return Status::OK();
}

FailoverRouterStats FailoverRouter::GetStats() const noexcept {
  FailoverRouterStats stats;
  stats.total_requests = total_requests_.load(std::memory_order_relaxed);
  stats.primary_attempts = primary_attempts_.load(std::memory_order_relaxed);
  stats.primary_successes = primary_successes_.load(std::memory_order_relaxed);
  stats.primary_failures = primary_failures_.load(std::memory_order_relaxed);
  stats.failover_attempts = failover_attempts_.load(std::memory_order_relaxed);
  stats.direct_fallback_routes = direct_fallback_routes_.load(std::memory_order_relaxed);
  stats.rate_limit_skips = rate_limit_skips_.load(std::memory_order_relaxed);
  stats.fallback_successes = fallback_successes_.load(std::memory_order_relaxed);
  stats.fallback_failures = fallback_failures_.load(std::memory_order_relaxed);
  stats.all_providers_failed = all_providers_failed_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace tessera
