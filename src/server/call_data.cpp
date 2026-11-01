#include "tessera/server/call_data.hpp"
#include "tessera/common/correlation.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

ChatCompletionCallData::ChatCompletionCallData(
    ICompletionBackend* backend,
    ChatCompletionRequest request,
    CompletionCallback on_finish,
    size_t max_payload_bytes)
    : backend_(backend),
      request_(std::move(request)),
      on_finish_(std::move(on_finish)),
      max_payload_bytes_(max_payload_bytes) {}

void ChatCompletionCallData::Proceed(bool ok) {
  if (!ok) {
    state_ = CallState::DESTROY;
    if (on_finish_) {
      on_finish_(response_, Status::Cancelled("Call cancelled by client"));
    }
    return;
  }

  if (state_ == CallState::CREATE) {
    state_ = CallState::PROCESS;

    // Attach correlation ID if empty
    if (request_.correlation_id.empty()) {
      request_.correlation_id = CorrelationId::Generate();
    }

    TESSERA_LOG_DEBUG("Processing request with correlation ID: {}", request_.correlation_id);

    // Validate request constraints
    status_ = ValidateRequest(request_, max_payload_bytes_);
    if (!status_.ok()) {
      TESSERA_LOG_WARN("Validation failed for correlation {}: {}", request_.correlation_id, status_.message);
      state_ = CallState::FINISH;
      if (on_finish_) {
        on_finish_(response_, status_);
      }
      state_ = CallState::DESTROY;
      return;
    }

    // Execute backend completion
    if (backend_ != nullptr) {
      status_ = backend_->Complete(request_, &response_);
    } else {
      status_ = Status::Internal("No backend available to process completion");
    }

    state_ = CallState::FINISH;
    if (on_finish_) {
      on_finish_(response_, status_);
    }
    state_ = CallState::DESTROY;
  }
}

}  // namespace tessera

