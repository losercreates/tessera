#include "tessera/backend/mock_upstream_client.hpp"
#include <thread>
#include <utility>

namespace tessera {

MockUpstreamClient::MockUpstreamClient(
    std::string provider_name,
    std::string default_model,
    std::string endpoint)
    : provider_name_(std::move(provider_name)),
      default_model_(std::move(default_model)),
      endpoint_(std::move(endpoint)) {}

UpstreamResponse MockUpstreamClient::Execute(const UpstreamRequest& request) {
  if (simulated_latency_.count() > 0) {
    std::this_thread::sleep_for(simulated_latency_);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  recorded_requests_.push_back(request);

  if (!queued_responses_.empty()) {
    UpstreamResponse resp = queued_responses_.front();
    queued_responses_.pop_front();
    if (resp.model.empty()) {
      resp.model = request.model.empty() ? default_model_ : request.model;
    }
    return resp;
  }

  UpstreamResponse default_resp;
  default_resp.status_code = default_status_code_;
  default_resp.model = request.model.empty() ? default_model_ : request.model;
  default_resp.prompt_tokens = 32;
  default_resp.completion_tokens = 16;
  default_resp.total_tokens = 48;
  default_resp.latency_ms = static_cast<double>(simulated_latency_.count());

  if (default_status_code_ == 200) {
    default_resp.completion_text = default_response_text_;
    default_resp.body = R"({"id":"mock-cmpl-1","choices":[{"message":{"role":"assistant","content":")" +
                        default_response_text_ + R"("},"finish_reason":"stop"}],"usage":{"prompt_tokens":32,"completion_tokens":16,"total_tokens":48}})";
  } else if (default_status_code_ == 429) {
    default_resp.error_message = "Rate limit exceeded: TPM / RPM quota exhausted";
    default_resp.body = R"({"error":{"message":"Rate limit reached","type":"tokens","code":"rate_limit_exceeded"}})";
  } else if (default_status_code_ >= 500) {
    default_resp.error_message = "Upstream server error: 500 Internal Server Error";
    default_resp.body = R"({"error":{"message":"Internal server error","type":"server_error"}})";
  } else {
    default_resp.error_message = "Mock upstream error status " + std::to_string(default_status_code_);
  }

  return default_resp;
}

void MockUpstreamClient::EnqueueResponse(UpstreamResponse resp) {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_responses_.push_back(std::move(resp));
}

void MockUpstreamClient::EnqueueStatusCode(int status_code, std::string body) {
  UpstreamResponse resp;
  resp.status_code = status_code;
  resp.body = std::move(body);
  if (status_code == 200) {
    resp.completion_text = default_response_text_;
  } else if (status_code == 429) {
    resp.error_message = "Rate limit exceeded";
  } else {
    resp.error_message = "Server error " + std::to_string(status_code);
  }
  EnqueueResponse(resp);
}

size_t MockUpstreamClient::CallCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recorded_requests_.size();
}

std::vector<UpstreamRequest> MockUpstreamClient::RecordedRequests() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recorded_requests_;
}

void MockUpstreamClient::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_responses_.clear();
  recorded_requests_.clear();
  default_status_code_ = 200;
  simulated_latency_ = std::chrono::milliseconds(0);
}

}  // namespace tessera

