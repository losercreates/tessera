#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/backend/upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/routing/circuit_types.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tessera {

struct CascadeRouterStats {
  uint64_t total_requests = 0;
  uint64_t successful_requests = 0;
  uint64_t failed_requests = 0;
  uint64_t failover_attempts = 0;
  uint64_t direct_bypasses = 0;
  uint64_t all_providers_failed = 0;
};

class CascadeRouter : public ICompletionBackend {
 public:
  CascadeRouter(std::vector<CascadeTarget> targets, std::string route_id = "default");
  ~CascadeRouter() override = default;

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "CascadeRouter";
  }

  [[nodiscard]] const std::string& RouteId() const noexcept {
    return route_id_;
  }

  [[nodiscard]] size_t TargetCount() const noexcept {
    return targets_.size();
  }

  [[nodiscard]] const std::vector<CascadeTarget>& Targets() const noexcept {
    return targets_;
  }

  [[nodiscard]] CascadeRouterStats GetStats() const;
  void ResetStats();

 private:
  std::vector<CascadeTarget> targets_;
  std::string route_id_;
  mutable std::mutex stats_mutex_;
  CascadeRouterStats stats_;
};

}  // namespace tessera
