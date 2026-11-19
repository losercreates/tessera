#include <gtest/gtest.h>
#include "tessera/routing/cascade_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"

using namespace tessera;

class CascadeRouterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    CircuitBreakerConfig breaker_cfg;
    breaker_cfg.consecutive_failure_threshold = 2;
    breaker_cfg.base_cooldown_ms = 1000;
    breaker_cfg.max_cooldown_ms = 60000;
    breaker_cfg.trip_on_429 = true;

    tier1_client_ = std::make_shared<MockUpstreamClient>("tier1", "model1", "http://tier1");
    tier1_breaker_ = std::make_shared<CircuitBreaker>(breaker_cfg);

    tier2_client_ = std::make_shared<MockUpstreamClient>("tier2", "model2", "http://tier2");
    tier2_breaker_ = std::make_shared<CircuitBreaker>(breaker_cfg);

    tier3_client_ = std::make_shared<MockUpstreamClient>("tier3", "model3", "http://tier3");
    tier3_breaker_ = std::make_shared<CircuitBreaker>(breaker_cfg);

    std::vector<CascadeTarget> targets = {
        CascadeTarget{.client = tier1_client_, .breaker = tier1_breaker_, .target_model = "model1", .priority = 0},
        CascadeTarget{.client = tier2_client_, .breaker = tier2_breaker_, .target_model = "model2", .priority = 1},
        CascadeTarget{.client = tier3_client_, .breaker = tier3_breaker_, .target_model = "model3", .priority = 2},
    };

    router_ = std::make_shared<CascadeRouter>(targets, "3-tier-test");
  }

  std::shared_ptr<MockUpstreamClient> tier1_client_;
  std::shared_ptr<CircuitBreaker> tier1_breaker_;
  std::shared_ptr<MockUpstreamClient> tier2_client_;
  std::shared_ptr<CircuitBreaker> tier2_breaker_;
  std::shared_ptr<MockUpstreamClient> tier3_client_;
  std::shared_ptr<CircuitBreaker> tier3_breaker_;
  std::shared_ptr<CascadeRouter> router_;
};

TEST_F(CascadeRouterTest, PrimarySuccessBypassesFallbacks) {
  tier1_client_->SetDefaultResponseText("Tier 1 response");

  ChatCompletionRequest req;
  req.correlation_id = "test-1";
  req.messages.push_back(ChatMessage{.role = "user", .content = "hello"});

  ChatCompletionResponse resp;
  Status st = router_->Complete(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp.choices[0].message.content, "Tier 1 response");
  EXPECT_EQ(tier1_client_->CallCount(), 1u);
  EXPECT_EQ(tier2_client_->CallCount(), 0u);
  EXPECT_EQ(tier3_client_->CallCount(), 0u);

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.total_requests, 1u);
  EXPECT_EQ(stats.successful_requests, 1u);
  EXPECT_EQ(stats.failover_attempts, 0u);
}

TEST_F(CascadeRouterTest, TripsAcrossNTiersWithZeroLoss) {
  // Script Tier 1 to 429 (immediate trip)
  tier1_client_->EnqueueStatusCode(429, "Rate limit exceeded");
  // Script Tier 2 to 500
  tier2_client_->EnqueueStatusCode(500, "Internal Server Error");
  // Script Tier 3 to 200
  tier3_client_->SetDefaultResponseText("Tier 3 recovered response");

  ChatCompletionRequest req;
  req.correlation_id = "test-cascade";
  req.messages.push_back(ChatMessage{.role = "user", .content = "solve problem"});

  ChatCompletionResponse resp;
  Status st = router_->Complete(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp.choices[0].message.content, "Tier 3 recovered response");

  EXPECT_EQ(tier1_client_->CallCount(), 1u);
  EXPECT_EQ(tier2_client_->CallCount(), 1u);
  EXPECT_EQ(tier3_client_->CallCount(), 1u);

  // Tier 1 breaker must be OPEN from 429
  EXPECT_EQ(tier1_breaker_->GetState(), CircuitState::OPEN);

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.total_requests, 1u);
  EXPECT_EQ(stats.successful_requests, 1u);
  EXPECT_EQ(stats.failover_attempts, 2u);

  // Subsequent request: Tier 1 is OPEN, so it must be directly bypassed!
  ChatCompletionResponse resp2;
  st = router_->Complete(req, &resp2);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(tier1_client_->CallCount(), 1u); // Tier 1 not called!
  EXPECT_EQ(tier2_client_->CallCount(), 2u); // Started from Tier 2

  stats = router_->GetStats();
  EXPECT_EQ(stats.total_requests, 2u);
  EXPECT_EQ(stats.direct_bypasses, 1u);
}

TEST_F(CascadeRouterTest, AllProvidersExhaustedReturnsUnavailable) {
  tier1_client_->SetDefaultStatusCode(503);
  tier2_client_->SetDefaultStatusCode(503);
  tier3_client_->SetDefaultStatusCode(503);

  ChatCompletionRequest req;
  req.correlation_id = "test-all-down";
  req.messages.push_back(ChatMessage{.role = "user", .content = "ping"});

  ChatCompletionResponse resp;
  Status st = router_->Complete(req, &resp);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code, StatusCode::UNAVAILABLE);
  EXPECT_NE(st.message.find("exhausted"), std::string::npos);

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.all_providers_failed, 1u);
  EXPECT_EQ(stats.failed_requests, 1u);
}

TEST_F(CascadeRouterTest, RejectsNullResponsePointer) {
  ChatCompletionRequest req;
  Status st = router_->Complete(req, nullptr);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code, StatusCode::INVALID_ARGUMENT);
}
