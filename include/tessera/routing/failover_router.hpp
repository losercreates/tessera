#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/backend/upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace tessera {

class FailoverRouter : public ICompletionBackend {
 public:
  FailoverRouter(
      std::shared_ptr<IUpstreamClient> primary_client,
      std::shared_ptr<ICircuitBreaker> primary_breaker,
      std::shared_ptr<IUpstreamClient> fallback_client,
      std::shared_ptr<ICircuitBreaker> fallback_breaker,
      std::string fallback_model = "google/gemma-2-9b-it");

  ~FailoverRouter() override = default;

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "FailoverRouter";
  }

  [[nodiscard]] std::shared_ptr<ICircuitBreaker> PrimaryBreaker() const noexcept {
    return primary_breaker_;
  }

  [[nodiscard]] std::shared_ptr<ICircuitBreaker> FallbackBreaker() const noexcept {
    return fallback_breaker_;
  }

  [[nodiscard]] FailoverRouterStats GetStats() const noexcept;

 private:
  UpstreamRequest BuildUpstreamRequest(
      const ChatCompletionRequest& req,
      std::string_view provider,
      std::string_view model) const;

  void PopulateResponse(
      const UpstreamResponse& upstream_resp,
      const ChatCompletionRequest& orig_req,
      ChatCompletionResponse* response) const;

  std::shared_ptr<IUpstreamClient> primary_client_;
  std::shared_ptr<ICircuitBreaker> primary_breaker_;
  std::shared_ptr<IUpstreamClient> fallback_client_;
  std::shared_ptr<ICircuitBreaker> fallback_breaker_;
  std::string fallback_model_;

  // Atomic statistics
  mutable std::atomic<uint64_t> total_requests_{0};
  mutable std::atomic<uint64_t> primary_attempts_{0};
  mutable std::atomic<uint64_t> primary_successes_{0};
  mutable std::atomic<uint64_t> primary_failures_{0};
  mutable std::atomic<uint64_t> failover_attempts_{0};
  mutable std::atomic<uint64_t> direct_fallback_routes_{0};
  mutable std::atomic<uint64_t> fallback_successes_{0};
  mutable std::atomic<uint64_t> fallback_failures_{0};
  mutable std::atomic<uint64_t> all_providers_failed_{0};
};

}  // namespace tessera

