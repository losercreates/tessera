#include "tessera/routing/circuit_breaker.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace tessera {

class CircuitBreakerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.consecutive_failure_threshold = 3;
    config_.base_cooldown_ms = 100;
    config_.max_cooldown_ms = 1000;
    config_.trip_on_429 = true;
    breaker_ = std::make_unique<CircuitBreaker>(config_);
  }

  CircuitBreakerConfig config_;
  std::unique_ptr<CircuitBreaker> breaker_;
};

TEST_F(CircuitBreakerTest, InitialStateIsClosed) {
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_->CanAttempt());
  auto stats = breaker_->GetStats();
  EXPECT_EQ(stats.current_state, CircuitState::CLOSED);
  EXPECT_EQ(stats.total_requests, 1u);
  EXPECT_EQ(stats.consecutive_failures, 0u);
}

TEST_F(CircuitBreakerTest, ImmediateTripOn429) {
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);

  // Constitution Principle II: A single HTTP 429 trips breaker immediately to OPEN
  breaker_->RecordFailure(429, "Too Many Requests / Quota Exceeded");

  EXPECT_EQ(breaker_->GetState(), CircuitState::OPEN);
  EXPECT_FALSE(breaker_->CanAttempt());

  auto stats = breaker_->GetStats();
  EXPECT_EQ(stats.trip_count, 1u);
  EXPECT_EQ(stats.rejected_requests, 1u);
}

TEST_F(CircuitBreakerTest, TripsOnConsecutive5xxThreshold) {
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);

  // 1st error
  breaker_->RecordFailure(500, "Internal Server Error");
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_->CanAttempt());

  // 2nd error
  breaker_->RecordFailure(503, "Service Unavailable");
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_->CanAttempt());

  // Intervening success resets streak
  breaker_->RecordSuccess();
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_EQ(breaker_->GetStats().consecutive_failures, 0u);

  // 1st error again
  breaker_->RecordFailure(502, "Bad Gateway");
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);

  // 2nd error
  breaker_->RecordFailure(504, "Gateway Timeout");
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);

  // 3rd error: hits threshold -> trips to OPEN
  breaker_->RecordFailure(500, "Internal Server Error");
  EXPECT_EQ(breaker_->GetState(), CircuitState::OPEN);
  EXPECT_FALSE(breaker_->CanAttempt());
}

TEST_F(CircuitBreakerTest, ManualResetRestoresClosed) {
  breaker_->RecordFailure(429, "Rate limit exceeded");
  EXPECT_EQ(breaker_->GetState(), CircuitState::OPEN);
  EXPECT_FALSE(breaker_->CanAttempt());

  breaker_->Reset();
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_->CanAttempt());
  EXPECT_EQ(breaker_->GetStats().consecutive_failures, 0u);
}

TEST_F(CircuitBreakerTest, SingleProbeInHalfOpen) {
  breaker_->ForceState(CircuitState::HALF_OPEN);
  EXPECT_EQ(breaker_->GetState(), CircuitState::HALF_OPEN);

  // First thread acquires the probe slot
  EXPECT_TRUE(breaker_->CanAttempt());

  // Concurrent request while probe is in flight is rejected/diverted
  EXPECT_FALSE(breaker_->CanAttempt());

  // Probe succeeds: circuit resets to CLOSED
  breaker_->RecordSuccess();
  EXPECT_EQ(breaker_->GetState(), CircuitState::CLOSED);
  EXPECT_TRUE(breaker_->CanAttempt());
}

TEST_F(CircuitBreakerTest, ProbeFailureReTrips) {
  breaker_->ForceState(CircuitState::HALF_OPEN);
  EXPECT_EQ(breaker_->GetState(), CircuitState::HALF_OPEN);

  EXPECT_TRUE(breaker_->CanAttempt());

  // Probe fails: re-trips to OPEN
  breaker_->RecordFailure(500, "Probe failed");
  EXPECT_EQ(breaker_->GetState(), CircuitState::OPEN);
  EXPECT_FALSE(breaker_->CanAttempt());
}

TEST_F(CircuitBreakerTest, JitteredBackoffWithinBounds) {
  for (uint32_t failures = 1; failures <= 10; ++failures) {
    for (int i = 0; i < 50; ++i) {
      auto backoff = breaker_->CalculateBackoffWithJitter(failures);
      EXPECT_GE(backoff.count(), 0);
      uint64_t max_expected = std::min<uint64_t>(
          config_.max_cooldown_ms,
          static_cast<uint64_t>(config_.base_cooldown_ms) * (1ULL << std::min<uint32_t>(failures, 20)));
      EXPECT_LE(static_cast<uint64_t>(backoff.count()), max_expected);
    }
  }
}

TEST_F(CircuitBreakerTest, JitterTruncatesAtMaxCooldown) {
  for (int i = 0; i < 50; ++i) {
    auto backoff = breaker_->CalculateBackoffWithJitter(50);
    EXPECT_GE(backoff.count(), 0);
    EXPECT_LE(static_cast<uint64_t>(backoff.count()), static_cast<uint64_t>(config_.max_cooldown_ms));
  }
}

}  // namespace tessera

