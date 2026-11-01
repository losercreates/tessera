#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <string_view>
#include <ostream>

namespace tessera {

enum class StatusCode {
  OK = 0,
  CANCELLED = 1,
  UNKNOWN = 2,
  INVALID_ARGUMENT = 3,
  DEADLINE_EXCEEDED = 4,
  NOT_FOUND = 5,
  ALREADY_EXISTS = 6,
  PERMISSION_DENIED = 7,
  RESOURCE_EXHAUSTED = 8,
  FAILED_PRECONDITION = 9,
  ABORTED = 10,
  OUT_OF_RANGE = 11,
  UNIMPLEMENTED = 12,
  INTERNAL = 13,
  UNAVAILABLE = 14,
  DATA_LOSS = 15,
  UNAUTHENTICATED = 16
};

std::string_view StatusCodeToString(StatusCode code);

inline std::ostream& operator<<(std::ostream& os, StatusCode code) {
  return os << StatusCodeToString(code);
}

struct Status {
  StatusCode code{StatusCode::OK};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::OK; }

  static Status OK() { return Status{StatusCode::OK, ""}; }
  static Status Cancelled(std::string msg) {
    return Status{StatusCode::CANCELLED, std::move(msg)};
  }
  static Status InvalidArgument(std::string msg) {
    return Status{StatusCode::INVALID_ARGUMENT, std::move(msg)};
  }
  static Status ResourceExhausted(std::string msg) {
    return Status{StatusCode::RESOURCE_EXHAUSTED, std::move(msg)};
  }
  static Status Unavailable(std::string msg) {
    return Status{StatusCode::UNAVAILABLE, std::move(msg)};
  }
  static Status Internal(std::string msg) {
    return Status{StatusCode::INTERNAL, std::move(msg)};
  }
};

struct ChatMessage {
  std::string role;          // "system", "user", "assistant", "tool"
  std::string content;       // Text payload
  std::string name{""};      // Optional author name

  [[nodiscard]] bool IsValidRole() const noexcept {
    return role == "system" || role == "user" || role == "assistant" || role == "tool";
  }
};

struct ChatCompletionRequest {
  std::string model;
  std::vector<ChatMessage> messages;
  std::optional<float> temperature{1.0f};
  std::optional<uint32_t> max_tokens{2048};
  bool stream{false};
  std::string correlation_id;

  // Approximate byte size of the request payload
  [[nodiscard]] size_t ByteSize() const noexcept;
};

struct Choice {
  uint32_t index{0};
  ChatMessage message;
  std::string finish_reason{"stop"};
};

struct Usage {
  uint32_t prompt_tokens{0};
  uint32_t completion_tokens{0};
  uint32_t total_tokens{0};
};

struct ChatCompletionResponse {
  std::string id;
  int64_t created{0};
  std::string model;
  std::vector<Choice> choices;
  Usage usage;
  std::string correlation_id;
};

enum class HealthStatus {
  UNKNOWN = 0,
  SERVING = 1,
  NOT_SERVING = 2
};

std::string_view HealthStatusToString(HealthStatus status);

inline std::ostream& operator<<(std::ostream& os, HealthStatus status) {
  return os << HealthStatusToString(status);
}

// Request validation function enforcing spec and data-model constraints
Status ValidateRequest(const ChatCompletionRequest& req, size_t max_payload_bytes = 4 * 1024 * 1024);

}  // namespace tessera
