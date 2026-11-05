#include "tessera/routing/failover_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

namespace tessera {

class ChaosMockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    groq_client_ = std::make_shared<MockUpstreamClient>(
        "groq", "llama-3.3-70b-versatile", "https://api.groq.com/openai/v1/chat/completions");
    openrouter_client_ = std::make_shared<MockUpstreamClient>(
        "openrouter", "google/gemma-2-9b-it", "https://openrouter.ai/api/v1/chat/completions");

    groq_client_->SetDefaultResponseText("Groq LLM output");
    openrouter_client_->SetDefaultResponseText("OpenRouter LLM output");

    CircuitBreakerConfig groq_config;
    groq_config.consecutive_failure_threshold = 3;
    groq_config.base_cooldown_ms = 10;
    groq_config.max_cooldown_ms = 100;
    groq_config.trip_on_429 = true;
    groq_breaker_ = std::make_shared<CircuitBreaker>(groq_config);

    CircuitBreakerConfig openrouter_config;
    openrouter_config.consecutive_failure_threshold = 3;
    openrouter_config.base_cooldown_ms = 10;
    openrouter_config.max_cooldown_ms = 100;
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

TEST_F(ChaosMockTest, ConcurrencyStressWithIntermittent429And500) {
  constexpr size_t kThreads = 20;
  constexpr size_t kRequestsPerThread = 50;
  constexpr size_t kTotalRequests = kThreads * kRequestsPerThread;

  // Prime Groq with a sequence of intermittent 429 and 500 errors
  for (size_t i = 0; i < kTotalRequests; ++i) {
    if (i % 7 == 0) {
      groq_client_->EnqueueStatusCode(429, "Rate limit reached");
    } else if (i % 13 == 0) {
      groq_client_->EnqueueStatusCode(500, "Internal error");
    } else {
      groq_client_->EnqueueStatusCode(200, "Groq Success");
    }
  }

  std::vector<std::thread> workers;
  std::atomic<size_t> successful_completions{0};
  std::atomic<size_t> failed_completions{0};

  auto start_time = std::chrono::high_resolution_clock::now();

  for (size_t t = 0; t < kThreads; ++t) {
    workers.emplace_back([this, t, &successful_completions, &failed_completions]() {
      for (size_t r = 0; r < kRequestsPerThread; ++r) {
        ChatCompletionRequest req;
        req.correlation_id = "chaos-" + std::to_string(t) + "-" + std::to_string(r);
        req.messages.push_back(ChatMessage{"user", "Chaos test payload"});

        ChatCompletionResponse resp;
        Status s = router_->Complete(req, &resp);

        if (s.ok() && !resp.choices.empty()) {
          successful_completions.fetch_add(1);
        } else {
          failed_completions.fetch_add(1);
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  double total_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  double avg_overhead_ms = total_ms / static_cast<double>(kTotalRequests);

  // 100% aggregate success rate despite 429/500 storm on primary (SC-001)
  EXPECT_EQ(successful_completions.load(), kTotalRequests);
  EXPECT_EQ(failed_completions.load(), 0u);

  // Failover latency overhead is well within the < 5ms budget (SC-002)
  EXPECT_LT(avg_overhead_ms, 5.0);

  auto stats = router_->GetStats();
  EXPECT_EQ(stats.total_requests, kTotalRequests);
  EXPECT_GT(stats.failover_attempts + stats.direct_fallback_routes, 0u);
  EXPECT_GT(stats.fallback_successes, 0u);
}

}  // namespace tessera

