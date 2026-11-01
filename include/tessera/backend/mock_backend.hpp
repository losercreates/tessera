#pragma once

#include "tessera/backend/completion_backend.hpp"
#include <chrono>

namespace tessera {

class MockEchoBackend : public ICompletionBackend {
 public:
  explicit MockEchoBackend(std::chrono::milliseconds simulated_delay = std::chrono::milliseconds(0))
      : simulated_delay_(simulated_delay) {}

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override { return "MockEchoBackend"; }

  void SetSimulatedDelay(std::chrono::milliseconds delay) noexcept { simulated_delay_ = delay; }
  void SetSimulatedFailure(bool fail, StatusCode code = StatusCode::INTERNAL, std::string msg = "Simulated error") {
    simulate_failure_ = fail;
    failure_code_ = code;
    failure_msg_ = std::move(msg);
  }

 private:
  std::chrono::milliseconds simulated_delay_{0};
  bool simulate_failure_{false};
  StatusCode failure_code_{StatusCode::INTERNAL};
  std::string failure_msg_;
};

}  // namespace tessera

