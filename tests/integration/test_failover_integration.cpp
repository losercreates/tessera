#include "tessera/routing/failover_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

namespace tessera {

class FailoverIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    groq_client_ = std::make_shared<MockUpstreamClient>(
        "groq", "llama-3.3-70b-versatile", "https://api.groq.com/openai/v1/chat/completions");
    openrouter_client_ = std::make_shared<MockUpstreamClient>(
        "openrouter", "google/gemma-2-9b-it", "https://openrouter.ai/api/v1/chat/completions");

    groq_client_->SetDefaultResponseText("Response from Groq Llama 3.3 70B");
    openrouter_client_->SetDefaultResponseText("Response from OpenRouter Gemma-2-9B");

    CircuitBreakerConfig groq_config;
    groq_config.consecutive_failure_threshold = 3;
    groq_config.base_cooldown_ms = 50;
    groq_config.max_cooldown_ms = 500;
    groq_config.trip_on_429 = true;
    groq_breaker_ = std::make_shared<CircuitBreaker>(groq_config);

    CircuitBreakerConfig openrouter_config;
    openrouter_config.consecutive_failure_threshold = 3;
    openrouter_config.base_cooldown_ms = 50;
    openrouter_config.max_cooldown_ms = 500;
    openrouter_config.trip_on_429 = true;
    openrouter_breaker_ = std::make_shared<CircuitBreaker>(openrouter_config);

    router_ = std::make_unique<FailoverRouter>(
        groq_client_, groq_breaker_, openrouter_client_, openrouter_breaker_, "google/gemma-2-9b-it");
  }

  std::shared_ptr<MockUpstreamClient> groq_client_;
  std::shared_ptr<CircuitBreaker> groq_breaker_;
  std::shared_ptr<MockUpstreamClient> openrouter_client_;
  std::shared_ptr<CircuitBreaker> openrouter_breaker_;
  std::unique_ptr<FailoverRouter> router_;
};

TEST_F(FailoverIntegrationTest, NormalRoutingToPrimary) {
  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  req.messages.push_back(ChatMessage{"user", "Hello world"});
  req.correlation_id = "test-corr-1";

  ChatCompletionResponse resp;
  Status s = router_->Complete(req, &resp);

  EXPECT_TRUE(s.ok());
  EXPECT_EQ(resp.model, "llama-3.3-70b-versatile");
  EXPECT_EQ(resp.choices[0].message.content, "Response from Groq Llama 3.3 70B");
  EXPECT_EQ(groq_client_->CallCount(), 1u);
  EXPECT_EQ(openrouter_client_->CallCount(), 0u);
  EXPECT_EQ(groq_breaker_->GetState(), CircuitState::CLOSED);
}

TEST_F(FailoverIntegrationTest, ZeroLossFailoverOn429) {
  // Prime: first request succeeds on Groq
  ChatCompletionRequest req1;
  req1.correlation_id = "test-corr-prime";
  req1.messages.push_back(ChatMessage{"user", "First message"});
  ChatCompletionResponse resp1;
  EXPECT_TRUE(router_->Complete(req1, &resp1).ok());
  EXPECT_EQ(groq_client_->CallCount(), 1u);

  // Script Groq to return HTTP 429 quota exhaustion
  groq_client_->EnqueueStatusCode(429, R"({"error":{"message":"Rate limit exceeded"}})");

  ChatCompletionRequest req2;
  req2.correlation_id = "test-corr-failover";
  req2.messages.push_back(ChatMessage{"user", "Second message"});
  ChatCompletionResponse resp2;

  // Request should fail over transparently with zero dropped calls (SC-001)
  Status s2 = router_->Complete(req2, &resp2);
  EXPECT_TRUE(s2.ok());
  EXPECT_EQ(resp2.model, "google/gemma-2-9b-it");
  EXPECT_EQ(resp2.choices[0].message.content, "Response from OpenRouter Gemma-2-9B");

  // Groq circuit breaker must be tripped to OPEN immediately (SC-003)
  EXPECT_EQ(groq_breaker_->GetState(), CircuitState::OPEN);
  EXPECT_EQ(groq_client_->CallCount(), 2u);
  EXPECT_EQ(openrouter_client_->CallCount(), 1u);

  // Subsequent request routes directly to fallback without calling Groq
  ChatCompletionRequest req3;
  req3.correlation_id = "test-corr-direct-fallback";
  req3.messages.push_back(ChatMessage{"user", "Third message"});
  ChatCompletionResponse resp3;

  Status s3 = router_->Complete(req3, &resp3);
  EXPECT_TRUE(s3.ok());
  EXPECT_EQ(resp3.model, "google/gemma-2-9b-it");
  EXPECT_EQ(groq_client_->CallCount(), 2u);        // Groq not called!
  EXPECT_EQ(openrouter_client_->CallCount(), 2u);  // Directly to OpenRouter

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.total_requests, 3u);
  EXPECT_EQ(stats.failover_attempts, 1u);
  EXPECT_EQ(stats.direct_fallback_routes, 1u);
  EXPECT_EQ(stats.fallback_successes, 2u);
}

TEST_F(FailoverIntegrationTest, ZeroLossFailoverOn5xx) {
  // Script Groq to return 500 error
  groq_client_->EnqueueStatusCode(500, "Internal Server Error");

  ChatCompletionRequest req;
  req.correlation_id = "test-corr-500";
  req.messages.push_back(ChatMessage{"user", "Calculate fibonacci"});
  ChatCompletionResponse resp;

  Status s = router_->Complete(req, &resp);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(resp.model, "google/gemma-2-9b-it");
  EXPECT_EQ(openrouter_client_->CallCount(), 1u);
  EXPECT_EQ(groq_breaker_->GetStats().consecutive_failures, 1u);
}

TEST_F(FailoverIntegrationTest, DualProviderExhaustionReturnsUnavailable) {
  // Both Groq and OpenRouter fail
  groq_client_->SetDefaultStatusCode(503);
  openrouter_client_->SetDefaultStatusCode(503);

  ChatCompletionRequest req;
  req.correlation_id = "test-corr-both-down";
  req.messages.push_back(ChatMessage{"user", "Emergency ping"});
  ChatCompletionResponse resp;

  Status s = router_->Complete(req, &resp);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code, StatusCode::UNAVAILABLE);

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.all_providers_failed, 1u);
}

TEST_F(FailoverIntegrationTest, SingleProbeRecoveryInHalfOpen) {
  // Trip Groq breaker to OPEN
  groq_breaker_->ForceState(CircuitState::HALF_OPEN);

  // Setup Groq to return success on probe with 50ms latency to hold probe in flight
  groq_client_->SetDefaultResponseText("Groq has recovered!");
  groq_client_->SetSimulatedLatency(std::chrono::milliseconds(50));

  constexpr size_t kThreads = 10;
  std::vector<std::thread> workers;
  std::atomic<size_t> groq_hits{0};
  std::atomic<size_t> openrouter_hits{0};

  for (size_t i = 0; i < kThreads; ++i) {
    workers.emplace_back([this, i, &groq_hits, &openrouter_hits]() {
      ChatCompletionRequest req;
      req.correlation_id = "corr-probe-" + std::to_string(i);
      req.messages.push_back(ChatMessage{"user", "Concurrent probe test"});
      ChatCompletionResponse resp;
      Status s = router_->Complete(req, &resp);
      if (s.ok()) {
        if (resp.model == "llama-3.3-70b-versatile") {
          groq_hits.fetch_add(1);
        } else if (resp.model == "google/gemma-2-9b-it") {
          openrouter_hits.fetch_add(1);
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }
  groq_client_->SetSimulatedLatency(std::chrono::milliseconds(0));

  // Exactly 1 probe was permitted through to Groq, remaining threads went to OpenRouter
  EXPECT_EQ(groq_hits.load(), 1u);
  EXPECT_EQ(openrouter_hits.load(), kThreads - 1);
  EXPECT_EQ(groq_breaker_->GetState(), CircuitState::CLOSED);

  // Subsequent requests go to Groq
  ChatCompletionRequest post_req;
  post_req.correlation_id = "corr-post-recovery";
  post_req.messages.push_back(ChatMessage{"user", "Normal traffic resumed"});
  ChatCompletionResponse post_resp;
  EXPECT_TRUE(router_->Complete(post_req, &post_resp).ok());
  EXPECT_EQ(post_resp.model, "llama-3.3-70b-versatile");
}

}  // namespace tessera
