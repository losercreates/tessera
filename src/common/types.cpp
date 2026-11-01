#include "tessera/common/types.hpp"

namespace tessera {

std::string_view StatusCodeToString(StatusCode code) {
  switch (code) {
    case StatusCode::OK: return "OK";
    case StatusCode::CANCELLED: return "CANCELLED";
    case StatusCode::UNKNOWN: return "UNKNOWN";
    case StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
    case StatusCode::NOT_FOUND: return "NOT_FOUND";
    case StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
    case StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
    case StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
    case StatusCode::ABORTED: return "ABORTED";
    case StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
    case StatusCode::INTERNAL: return "INTERNAL";
    case StatusCode::UNAVAILABLE: return "UNAVAILABLE";
    case StatusCode::DATA_LOSS: return "DATA_LOSS";
    case StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
  }
  return "UNKNOWN";
}

std::string_view HealthStatusToString(HealthStatus status) {
  switch (status) {
    case HealthStatus::SERVING: return "SERVING";
    case HealthStatus::NOT_SERVING: return "NOT_SERVING";
    case HealthStatus::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

size_t ChatCompletionRequest::ByteSize() const noexcept {
  size_t total = model.size() + correlation_id.size() + sizeof(bool) + sizeof(float) + sizeof(uint32_t);
  for (const auto& msg : messages) {
    total += msg.role.size() + msg.content.size() + msg.name.size();
  }
  return total;
}

Status ValidateRequest(const ChatCompletionRequest& req, size_t max_payload_bytes) {
  if (req.ByteSize() > max_payload_bytes) {
    return Status::ResourceExhausted(
        "Request size exceeds configured maximum limit of " + std::to_string(max_payload_bytes) + " bytes");
  }

  if (req.model.empty()) {
    return Status::InvalidArgument("Model identifier cannot be empty");
  }

  if (req.messages.empty()) {
    return Status::InvalidArgument("Request must contain at least one message");
  }

  if (req.temperature.has_value()) {
    float temp = req.temperature.value();
    if (temp < 0.0f || temp > 2.0f) {
      return Status::InvalidArgument("Temperature must be between 0.0 and 2.0");
    }
  }

  for (size_t i = 0; i < req.messages.size(); ++i) {
    const auto& msg = req.messages[i];
    if (!msg.IsValidRole()) {
      return Status::InvalidArgument(
          "Message at index " + std::to_string(i) + " has invalid role '" + msg.role +
          "'. Must be one of: system, user, assistant, tool");
    }
    if (msg.content.empty()) {
      return Status::InvalidArgument("Message at index " + std::to_string(i) + " content cannot be empty");
    }
  }

  return Status::OK();
}

}  // namespace tessera

