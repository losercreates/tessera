#include <gtest/gtest.h>
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/backend/mock_backend.hpp"
#include <thread>
#include <chrono>

using namespace tessera;

class RateLimiterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig config;
    config.min_connections = 2;
    config.max_connections = 4;
    redis_pool_ = std::make_shared<RedisConnectionPool>(config, mock_redis_);
    rate_limiter_ = std::make_shared<DistributedRateLimiter>(redis_pool_);
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
};

TEST_F(RateLimiterTest, InitialQuotaPermitsRequests) {
  rate_limiter_->LoadStandardPolicies();
  EXPECT_TRUE(rate_limiter_->HasRules("groq"));
  EXPECT_TRUE(rate_limiter_->HasRules("openrouter"));

  // Groq allows 30 RPM
  auto res = rate_limiter_->CheckAndConsume("groq", 1.0);
  EXPECT_TRUE(res.allowed);
  EXPECT_EQ(res.remaining_tokens, 29u);
  EXPECT_EQ(res.retry_after_ms, 0u);
}

TEST_F(RateLimiterTest, DepletionRejectsSubsequentRequests) {
  rate_limiter_->LoadStandardPolicies();

  // Consume all 30 tokens for groq minute window
  for (int i = 0; i < 30; ++i) {
    auto res = rate_limiter_->CheckAndConsume("groq", 1.0);
    EXPECT_TRUE(res.allowed);
  }

  // 31st request must be rejected
  auto rejected = rate_limiter_->CheckAndConsume("groq", 1.0);
  EXPECT_FALSE(rejected.allowed);
  EXPECT_EQ(rejected.remaining_tokens, 0u);
  EXPECT_GT(rejected.retry_after_ms, 0u);
}

TEST_F(RateLimiterTest, ProviderQuotasAreIsolated) {
  rate_limiter_->LoadStandardPolicies();

  // Exhaust OpenRouter quota (20 RPM)
  for (int i = 0; i < 20; ++i) {
    auto res = rate_limiter_->CheckAndConsume("openrouter", 1.0);
    EXPECT_TRUE(res.allowed);
  }

  // OpenRouter is now exhausted
  auto openrouter_rejected = rate_limiter_->CheckAndConsume("openrouter", 1.0);
  EXPECT_FALSE(openrouter_rejected.allowed);

  // Groq should still be completely unconstrained
  auto groq_allowed = rate_limiter_->CheckAndConsume("groq", 1.0);
  EXPECT_TRUE(groq_allowed.allowed);
  EXPECT_EQ(groq_allowed.remaining_tokens, 29u);
}

TEST_F(RateLimiterTest, FailOpenWhenRedisDown) {
  rate_limiter_->LoadStandardPolicies();

  mock_redis_->SetSimulateFailure(true);
  mock_redis_->Disconnect();
  redis_pool_->SetHealthy(false);

  // When Redis is unreachable, fail-open policy must allow requests
  auto res = rate_limiter_->CheckAndConsume("groq", 1.0);
  EXPECT_TRUE(res.allowed);
}

TEST_F(RateLimiterTest, RateLimitedBackendEnforcesLimit) {
  rate_limiter_->LoadStandardPolicies();

  auto inner = std::make_shared<MockEchoBackend>();
  auto rate_limited_backend = std::make_shared<RateLimitedBackend>(inner, rate_limiter_, "groq");

  ChatCompletionRequest req;
  req.model = "llama-3.3-70b-versatile";
  ChatMessage msg{.role = "user", .content = "Hello"};
  req.messages.push_back(msg);

  // 30 requests should succeed
  for (int i = 0; i < 30; ++i) {
    ChatCompletionResponse resp;
    Status st = rate_limited_backend->Complete(req, &resp);
    EXPECT_TRUE(st.ok());
  }

  // 31st request must return ResourceExhausted
  ChatCompletionResponse resp;
  Status st = rate_limited_backend->Complete(req, &resp);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code, StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(st.message.find("Rate limit exceeded for provider 'groq'"), std::string::npos);
}

TEST_F(RateLimiterTest, ProviderResolutionFromModel) {
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("llama-3.3-70b-versatile"), "groq");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("groq/compound"), "groq");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("google/gemma-2-9b-it"), "openrouter");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("openrouter/auto"), "openrouter");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("deepseek-chat"), "deepseek");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("ollama/phi3"), "ollama");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("mistral-large"), "mistral");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("gpt-4o"), "openai");
  EXPECT_EQ(RateLimitedBackend::ResolveProvider("custom-model", "groq"), "groq");
}

TEST_F(RateLimiterTest, CustomProviderResolverOverride) {
  auto mock_backend = std::make_shared<MockEchoBackend>();

  RateLimitedBackend backend(
      mock_backend, rate_limiter_, "default-prov",
      [](std::string_view model) -> std::string {
        if (model == "my-custom-model") return "custom-cluster";
        return "";
      });

  ChatCompletionRequest req;
  req.model = "my-custom-model";
  req.messages.push_back(ChatMessage{.role = "user", .content = "hello"});
  ChatCompletionResponse resp;
  Status st = backend.Complete(req, &resp);
  EXPECT_TRUE(st.ok());
}

TEST_F(RateLimiterTest, ZeroPreseedInitializationAndUnconstrainedPassThrough) {
  auto shared_state = mock_redis_->GetSharedState();
  EXPECT_FALSE(rate_limiter_->HasRules("groq"));
  EXPECT_FALSE(rate_limiter_->HasRules("openrouter"));
  EXPECT_TRUE(rate_limiter_->GetRules("groq").empty());
  EXPECT_TRUE(rate_limiter_->GetRules("openrouter").empty());

  auto res_groq = rate_limiter_->CheckAndConsume("groq", 1.0);
  auto res_custom = rate_limiter_->CheckAndConsume("unconfigured_provider", 1.0);
  EXPECT_TRUE(res_groq.allowed);
  EXPECT_TRUE(res_custom.allowed);
  EXPECT_EQ(shared_state->lua_eval_count.load(), 0u);

  rate_limiter_->AddRule(RateLimitRule{
      .provider = "ephemeral",
      .window_name = "minute",
      .capacity = 5.0,
      .refill_rate_per_sec = 5.0 / 60.0,
      .ttl_seconds = 60,
  });
  EXPECT_TRUE(rate_limiter_->HasRules("ephemeral"));

  rate_limiter_->ClearAllRules();
  EXPECT_FALSE(rate_limiter_->HasRules("ephemeral"));
  EXPECT_FALSE(rate_limiter_->HasRules("groq"));
  EXPECT_FALSE(rate_limiter_->HasRules("openrouter"));
}

