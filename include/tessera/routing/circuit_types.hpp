#pragma once

#include "tessera/common/types.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera {

enum class CircuitState : uint8_t {
  CLOSED = 0,     // Normal healthy state. Requests pass through.
  OPEN = 1,       // Tripped state. Requests diverted to fallback.
  HALF_OPEN = 2   // Trial recovery state. Exactly one probe request permitted.
};

[[nodiscard]] constexpr std::string_view ToString(CircuitState state) noexcept {
  switch (state) {
    case CircuitState::CLOSED: return "CLOSED";
    case CircuitState::OPEN: return "OPEN";
    case CircuitState::HALF_OPEN: return "HALF-OPEN";
  }
  return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, CircuitState state) {
  return os << ToString(state);
}

struct CircuitBreakerConfig {
  uint32_t consecutive_failure_threshold = 3;  // Trips after N consecutive 5xx or timeouts
  uint32_t base_cooldown_ms = 1000;            // Initial backoff window in ms
  uint32_t max_cooldown_ms = 60000;            // Truncated backoff ceiling in ms
  uint32_t request_timeout_ms = 10000;         // Upstream socket/request execution timeout
  bool trip_on_429 = true;                     // HTTP 429 trips circuit immediately on 1st occurrence
};

struct CircuitBreakerStats {
  CircuitState current_state = CircuitState::CLOSED;
  uint64_t total_requests = 0;
  uint64_t successful_requests = 0;
  uint64_t failed_requests = 0;
  uint64_t rejected_requests = 0;
  uint32_t consecutive_failures = 0;
  uint32_t trip_count = 0;
  uint32_t recovery_count = 0;
  uint64_t current_cooldown_ms = 0;
};

struct UpstreamRequest {
  std::string provider;                                // e.g. "groq", "openrouter"
  std::string model;                                   // e.g. "llama-3.3-70b-versatile", "google/gemma-2-9b-it"
  std::vector<ChatMessage> messages;                   // System, user, assistant messages
  float temperature = 0.7f;
  uint32_t max_tokens = 1024;
  std::string correlation_id;
  std::unordered_map<std::string, std::string> headers;
  std::chrono::milliseconds timeout{10000};
};

struct UpstreamResponse {
  int status_code = 0;                                 // HTTP status: 200, 429, 500, 503, 0 (timeout)
  std::string body;                                    // Raw response body
  std::string completion_text;                         // Extracted completion text
  std::string model;                                   // Model reported in response
  uint32_t prompt_tokens = 0;
  uint32_t completion_tokens = 0;
  uint32_t total_tokens = 0;
  double latency_ms = 0.0;
  std::string error_message;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return status_code >= 200 && status_code < 300;
  }
  [[nodiscard]] bool IsRateLimited() const noexcept {
    return status_code == 429;
  }
  [[nodiscard]] bool IsServerError() const noexcept {
    return status_code >= 500 || status_code == 0;
  }
};

struct UpstreamProviderConfig {
  std::string name = "groq";
  std::string default_model = "llama-3.3-70b-versatile";
  std::string endpoint = "https://api.groq.com/openai/v1/chat/completions";
  std::string api_key;
  CircuitBreakerConfig circuit_config;
};

struct FailoverRouterConfig {
  UpstreamProviderConfig primary;                      // Groq
  UpstreamProviderConfig fallback;                     // OpenRouter
  bool enable_failover = true;
};

struct FailoverRouterStats {
  uint64_t total_requests = 0;
  uint64_t primary_attempts = 0;
  uint64_t primary_successes = 0;
  uint64_t primary_failures = 0;
  uint64_t failover_attempts = 0;                      // In-flight transparent retries
  uint64_t direct_fallback_routes = 0;                 // Dispatched to fallback because primary was OPEN
  uint64_t fallback_successes = 0;
  uint64_t fallback_failures = 0;
  uint64_t all_providers_failed = 0;
};

}  // namespace tessera
