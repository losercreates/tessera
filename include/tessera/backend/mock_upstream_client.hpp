#pragma once

#include "tessera/backend/upstream_client.hpp"
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tessera {

class MockUpstreamClient : public IUpstreamClient {
 public:
  explicit MockUpstreamClient(
      std::string provider_name = "mock",
      std::string default_model = "llama-3.3-70b-versatile",
      std::string endpoint = "https://mock.api/v1/chat/completions");

  ~MockUpstreamClient() override = default;

  UpstreamResponse Execute(const UpstreamRequest& request) override;

  [[nodiscard]] std::string_view ProviderName() const noexcept override {
    return provider_name_;
  }

  [[nodiscard]] std::string_view Endpoint() const noexcept override {
    return endpoint_;
  }

  [[nodiscard]] std::string_view DefaultModel() const noexcept override {
    return default_model_;
  }

  // Canned response scripting
  void EnqueueResponse(UpstreamResponse resp);
  void EnqueueStatusCode(int status_code, std::string body = "");
  void SetDefaultStatusCode(int status_code) { default_status_code_ = status_code; }
  void SetDefaultResponseText(std::string text) { default_response_text_ = std::move(text); }
  void SetSimulatedLatency(std::chrono::milliseconds latency) { simulated_latency_ = latency; }

  // Inspection & assertion hooks
  [[nodiscard]] size_t CallCount() const;
  [[nodiscard]] std::vector<UpstreamRequest> RecordedRequests() const;
  void Reset();

 private:
  std::string provider_name_;
  std::string default_model_;
  std::string endpoint_;

  mutable std::mutex mutex_;
  std::deque<UpstreamResponse> queued_responses_;
  std::vector<UpstreamRequest> recorded_requests_;

  int default_status_code_{200};
  std::string default_response_text_{"Mock LLM completion response."};
  std::chrono::milliseconds simulated_latency_{0};
};

}  // namespace tessera

