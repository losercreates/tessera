#pragma once

#include "tessera/routing/circuit_types.hpp"
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <atomic>

namespace tessera {

class ICircuitBreaker {
 public:
  virtual ~ICircuitBreaker() = default;

  /**
   * Evaluates whether a request may be dispatched to the protected upstream.
   * - In CLOSED: returns true.
   * - In OPEN: returns false if cooldown window has not expired.
   * - In HALF-OPEN: atomically permits exactly ONE probe thread (returns true)
   *   while returning false to all concurrent callers.
   */
  [[nodiscard]] virtual bool CanAttempt() = 0;

  /**
   * Reports a successful upstream completion (HTTP 200).
   * Resets consecutive failures to 0 and transitions HALF-OPEN -> CLOSED.
   */
  virtual void RecordSuccess() = 0;

  /**
   * Reports an upstream execution failure (e.g. HTTP 429, 5xx, socket timeout).
   * - If status_code == 429: trips immediately to OPEN (Constitution Principle II).
   * - If consecutive failures >= threshold: trips to OPEN.
   * - If in HALF-OPEN: re-trips immediately to OPEN and doubles exponential backoff.
   */
  virtual void RecordFailure(int status_code, std::string_view error_message = "") = 0;

  /**
   * Returns current active circuit state (CLOSED, OPEN, HALF-OPEN).
   */
  [[nodiscard]] virtual CircuitState GetState() const noexcept = 0;

  /**
   * Retrieves atomic telemetry and operational statistics.
   */
  [[nodiscard]] virtual CircuitBreakerStats GetStats() const noexcept = 0;

  /**
   * Manually resets circuit to CLOSED and clears failure counters.
   */
  virtual void Reset() = 0;

  /**
   * Testing hook to inject or force a specific circuit state.
   */
  virtual void ForceState(CircuitState state) = 0;
};

class CircuitBreaker : public ICircuitBreaker {
 public:
  explicit CircuitBreaker(CircuitBreakerConfig config = {});
  ~CircuitBreaker() override = default;

  bool CanAttempt() override;
  void RecordSuccess() override;
  void RecordFailure(int status_code, std::string_view error_message = "") override;

  [[nodiscard]] CircuitState GetState() const noexcept override;
  [[nodiscard]] CircuitBreakerStats GetStats() const noexcept override;
  void Reset() override;
  void ForceState(CircuitState state) override;

  // Calculates truncated exponential backoff with full randomized jitter
  [[nodiscard]] std::chrono::milliseconds CalculateBackoffWithJitter(uint32_t failure_count) const;

 private:
  CircuitBreakerConfig config_;

  mutable std::mutex mutex_;
  std::atomic<CircuitState> state_{CircuitState::CLOSED};
  std::atomic<uint32_t> consecutive_failures_{0};
  std::atomic<bool> probe_in_flight_{false};
  std::chrono::steady_clock::time_point open_until_{std::chrono::steady_clock::time_point::min()};

  // Telemetry counters
  std::atomic<uint64_t> total_requests_{0};
  std::atomic<uint64_t> successful_requests_{0};
  std::atomic<uint64_t> failed_requests_{0};
  std::atomic<uint64_t> rejected_requests_{0};
  std::atomic<uint32_t> trip_count_{0};
  std::atomic<uint32_t> recovery_count_{0};
  std::atomic<uint64_t> current_cooldown_ms_{0};
};

}  // namespace tessera

