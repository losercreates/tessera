#pragma once

#include "tessera/common/types.hpp"
#include <atomic>

namespace tessera {

class HealthService {
 public:
  explicit HealthService(HealthStatus initial_status = HealthStatus::SERVING)
      : status_(initial_status) {}

  void SetStatus(HealthStatus status) noexcept {
    status_.store(status, std::memory_order_release);
  }

  [[nodiscard]] HealthStatus GetStatus() const noexcept {
    return status_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool IsServing() const noexcept {
    return GetStatus() == HealthStatus::SERVING;
  }

 private:
  std::atomic<HealthStatus> status_{HealthStatus::SERVING};
};

}  // namespace tessera

