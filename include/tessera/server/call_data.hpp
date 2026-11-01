#pragma once

#include "tessera/common/types.hpp"
#include "tessera/backend/completion_backend.hpp"
#include <memory>
#include <functional>

namespace tessera {

enum class CallState {
  CREATE = 0,
  PROCESS = 1,
  FINISH = 2,
  DESTROY = 3
};

class ICallData {
 public:
  virtual ~ICallData() = default;
  virtual void Proceed(bool ok) = 0;
  [[nodiscard]] virtual CallState State() const noexcept = 0;
};

class ChatCompletionCallData : public ICallData {
 public:
  using CompletionCallback = std::function<void(const ChatCompletionResponse&, Status)>;

  ChatCompletionCallData(
      ICompletionBackend* backend,
      ChatCompletionRequest request,
      CompletionCallback on_finish,
      size_t max_payload_bytes = 4 * 1024 * 1024);

  void Proceed(bool ok) override;
  [[nodiscard]] CallState State() const noexcept override { return state_; }

  [[nodiscard]] const ChatCompletionRequest& Request() const noexcept { return request_; }
  [[nodiscard]] const ChatCompletionResponse& Response() const noexcept { return response_; }
  [[nodiscard]] const Status& LastStatus() const noexcept { return status_; }

 private:
  CallState state_{CallState::CREATE};
  ICompletionBackend* backend_{nullptr};
  ChatCompletionRequest request_;
  ChatCompletionResponse response_;
  Status status_{Status::OK()};
  CompletionCallback on_finish_;
  size_t max_payload_bytes_{4 * 1024 * 1024};
};

}  // namespace tessera

