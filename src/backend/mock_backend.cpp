#include "tessera/backend/mock_backend.hpp"
#include "tessera/common/correlation.hpp"
#include <thread>
#include <chrono>

namespace tessera {

Status MockEchoBackend::Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  if (simulate_failure_) {
    return Status{failure_code_, failure_msg_};
  }

  if (simulated_delay_.count() > 0) {
    std::this_thread::sleep_for(simulated_delay_);
  }

  std::string last_user_content;
  uint32_t prompt_tokens = 0;
  for (const auto& msg : request.messages) {
    prompt_tokens += static_cast<uint32_t>((msg.content.size() / 4) + 1);
    if (msg.role == "user") {
      last_user_content = msg.content;
    }
  }

  std::string response_content = "Echo response from Tessera Gateway: " + last_user_content;
  uint32_t completion_tokens = static_cast<uint32_t>((response_content.size() / 4) + 1);

  response->id = "chatcmpl-" + CorrelationId::Generate();
  response->created = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  response->model = request.model;
  response->correlation_id = request.correlation_id.empty() ? CorrelationId::Generate() : request.correlation_id;

  Choice choice;
  choice.index = 0;
  choice.message.role = "assistant";
  choice.message.content = response_content;
  choice.finish_reason = "stop";

  response->choices.clear();
  response->choices.push_back(std::move(choice));

  response->usage.prompt_tokens = prompt_tokens;
  response->usage.completion_tokens = completion_tokens;
  response->usage.total_tokens = prompt_tokens + completion_tokens;

  return Status::OK();
}

}  // namespace tessera

