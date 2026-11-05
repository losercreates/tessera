#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>
#include <random>

namespace tessera {

CircuitBreaker::CircuitBreaker(CircuitBreakerConfig config)
    : config_(config) {}

bool CircuitBreaker::CanAttempt() {
  total_requests_.fetch_add(1, std::memory_order_relaxed);
  CircuitState current = state_.load(std::memory_order_acquire);

  if (current == CircuitState::CLOSED) {
    return true;
  }

  auto now = std::chrono::steady_clock::now();

  if (current == CircuitState::OPEN) {
    bool cooldown_elapsed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cooldown_elapsed = (now >= open_until_);
    }

    if (!cooldown_elapsed) {
      rejected_requests_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Cooldown elapsed. Attempt transition OPEN -> HALF_OPEN
    CircuitState expected = CircuitState::OPEN;
    if (state_.compare_exchange_strong(expected, CircuitState::HALF_OPEN, std::memory_order_acq_rel)) {
      TESSERA_LOG_INFO("CircuitBreaker cooldown elapsed: Transitioning OPEN -> HALF-OPEN for probing");
    }
    current = state_.load(std::memory_order_acquire);
  }

  if (current == CircuitState::HALF_OPEN) {
    // Exactly one trial probe request is permitted through
    bool expected_probe = false;
    if (probe_in_flight_.compare_exchange_strong(expected_probe, true, std::memory_order_acq_rel)) {
      TESSERA_LOG_INFO("CircuitBreaker granted single trial probe in HALF-OPEN state");
      return true;
    }
    // Concurrent request while probe in flight -> divert to fallback
    rejected_requests_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

void CircuitBreaker::RecordSuccess() {
  successful_requests_.fetch_add(1, std::memory_order_relaxed);
  CircuitState current = state_.load(std::memory_order_acquire);

  if (current == CircuitState::HALF_OPEN) {
    state_.store(CircuitState::CLOSED, std::memory_order_release);
    consecutive_failures_.store(0, std::memory_order_relaxed);
    probe_in_flight_.store(false, std::memory_order_release);
    recovery_count_.fetch_add(1, std::memory_order_relaxed);
    current_cooldown_ms_.store(0, std::memory_order_relaxed);
    TESSERA_LOG_INFO("CircuitBreaker probe succeeded: Transitioning HALF-OPEN -> CLOSED (Healthy)");
  } else if (current == CircuitState::CLOSED) {
    consecutive_failures_.store(0, std::memory_order_relaxed);
  }
}

void CircuitBreaker::RecordFailure(int status_code, std::string_view error_message) {
  failed_requests_.fetch_add(1, std::memory_order_relaxed);
  CircuitState current = state_.load(std::memory_order_acquire);

  bool should_trip = false;
  uint32_t current_failures = 0;

  if (status_code == 429 && config_.trip_on_429) {
    // Constitution Principle II: Instant trip to OPEN on HTTP 429
    consecutive_failures_.fetch_add(1, std::memory_order_relaxed);
    current_failures = consecutive_failures_.load(std::memory_order_relaxed);
    should_trip = true;
    TESSERA_LOG_WARN("CircuitBreaker encountered HTTP 429 (Quota Exhausted) - Instant trip to OPEN");
  } else if (current == CircuitState::HALF_OPEN) {
    // Probe failure in HALF_OPEN: immediately re-trip to OPEN
    consecutive_failures_.fetch_add(1, std::memory_order_relaxed);
    current_failures = consecutive_failures_.load(std::memory_order_relaxed);
    should_trip = true;
    TESSERA_LOG_WARN("CircuitBreaker trial probe failed with status {} - Re-tripping to OPEN", status_code);
  } else {
    // Consecutive 5xx or timeout failure
    current_failures = consecutive_failures_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (current_failures >= config_.consecutive_failure_threshold) {
      should_trip = true;
      TESSERA_LOG_WARN("CircuitBreaker reached {} consecutive failures - Tripping to OPEN", current_failures);
    }
  }

  if (should_trip) {
    auto cooldown = CalculateBackoffWithJitter(std::max<uint32_t>(1, current_failures));
    current_cooldown_ms_.store(static_cast<uint64_t>(cooldown.count()), std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_until_ = std::chrono::steady_clock::now() + cooldown;
    }

    probe_in_flight_.store(false, std::memory_order_release);
    state_.store(CircuitState::OPEN, std::memory_order_release);
    trip_count_.fetch_add(1, std::memory_order_relaxed);

    TESSERA_LOG_WARN(
        "CircuitBreaker state is now OPEN. Cooldown: {}ms, reason='{}'",
        cooldown.count(), error_message.empty() ? "Upstream failure" : error_message);
  } else if (current == CircuitState::HALF_OPEN) {
    probe_in_flight_.store(false, std::memory_order_release);
  }
}

CircuitState CircuitBreaker::GetState() const noexcept {
  return state_.load(std::memory_order_acquire);
}

CircuitBreakerStats CircuitBreaker::GetStats() const noexcept {
  CircuitBreakerStats stats;
  stats.current_state = state_.load(std::memory_order_acquire);
  stats.total_requests = total_requests_.load(std::memory_order_relaxed);
  stats.successful_requests = successful_requests_.load(std::memory_order_relaxed);
  stats.failed_requests = failed_requests_.load(std::memory_order_relaxed);
  stats.rejected_requests = rejected_requests_.load(std::memory_order_relaxed);
  stats.consecutive_failures = consecutive_failures_.load(std::memory_order_relaxed);
  stats.trip_count = trip_count_.load(std::memory_order_relaxed);
  stats.recovery_count = recovery_count_.load(std::memory_order_relaxed);
  stats.current_cooldown_ms = current_cooldown_ms_.load(std::memory_order_relaxed);
  return stats;
}

void CircuitBreaker::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.store(CircuitState::CLOSED, std::memory_order_release);
  consecutive_failures_.store(0, std::memory_order_relaxed);
  probe_in_flight_.store(false, std::memory_order_release);
  open_until_ = std::chrono::steady_clock::time_point::min();
  current_cooldown_ms_.store(0, std::memory_order_relaxed);
  TESSERA_LOG_INFO("CircuitBreaker manually reset to CLOSED");
}

void CircuitBreaker::ForceState(CircuitState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.store(state, std::memory_order_release);
  if (state == CircuitState::OPEN) {
    open_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.base_cooldown_ms);
  } else if (state == CircuitState::CLOSED) {
    consecutive_failures_.store(0, std::memory_order_relaxed);
    probe_in_flight_.store(false, std::memory_order_release);
    open_until_ = std::chrono::steady_clock::time_point::min();
  }
}

std::chrono::milliseconds CircuitBreaker::CalculateBackoffWithJitter(uint32_t failure_count) const {
  uint32_t shift = std::min<uint32_t>(failure_count, 20);
  uint64_t multiplier = 1ULL << shift;
  uint64_t calculated = static_cast<uint64_t>(config_.base_cooldown_ms) * multiplier;
  uint64_t capped = std::min<uint64_t>(calculated, config_.max_cooldown_ms);

  if (capped == 0) {
    return std::chrono::milliseconds(0);
  }

  // AWS Full Jitter: random uniform between 0 and capped backoff
  thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist(0, capped);
  uint64_t jittered = dist(rng);

  return std::chrono::milliseconds(jittered);
}

}  // namespace tessera

