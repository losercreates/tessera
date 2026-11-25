#include <gtest/gtest.h>
#include "tessera/routing/model_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"

using namespace tessera;

class ModelRouterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    CircuitBreakerConfig b_cfg;
    deepseek_client_ = std::make_shared<MockUpstreamClient>("deepseek", "deepseek-chat", "https://api.deepseek.com");
    deepseek_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    ollama_client_ = std::make_shared<MockUpstreamClient>("ollama", "llama3.2", "http://localhost:11434");
    ollama_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    default_client_ = std::make_shared<MockUpstreamClient>("default_prov", "default_model", "http://default");
    default_breaker_ = std::make_shared<CircuitBreaker>(b_cfg);

    std::vector<CascadeTarget> ds_targets = {
        CascadeTarget{.client = deepseek_client_, .breaker = deepseek_breaker_, .target_model = "deepseek-chat", .priority = 0}};
    auto ds_cascade = std::make_shared<CascadeRouter>(ds_targets, "deepseek-cascade");

    std::vector<CascadeTarget> ollama_targets = {
        CascadeTarget{.client = ollama_client_, .breaker = ollama_breaker_, .target_model = "", .priority = 0}};
    auto ollama_cascade = std::make_shared<CascadeRouter>(ollama_targets, "ollama-cascade");

    std::vector<CascadeTarget> def_targets = {
        CascadeTarget{.client = default_client_, .breaker = default_breaker_, .target_model = "default_model", .priority = 0}};
    auto def_cascade = std::make_shared<CascadeRouter>(def_targets, "default-cascade");

    router_ = std::make_shared<ModelRouter>(def_cascade);
    router_->RegisterRoute("deepseek-chat", ds_cascade);
    router_->RegisterRoute("ollama/*", ollama_cascade);
  }

  std::shared_ptr<MockUpstreamClient> deepseek_client_;
  std::shared_ptr<CircuitBreaker> deepseek_breaker_;
  std::shared_ptr<MockUpstreamClient> ollama_client_;
  std::shared_ptr<CircuitBreaker> ollama_breaker_;
  std::shared_ptr<MockUpstreamClient> default_client_;
  std::shared_ptr<CircuitBreaker> default_breaker_;
  std::shared_ptr<ModelRouter> router_;
};

TEST_F(ModelRouterTest, ExactMatchRouting) {
  deepseek_client_->SetDefaultResponseText("DeepSeek response");

  ChatCompletionRequest req;
  req.model = "deepseek-chat";
  req.messages.push_back(ChatMessage{.role = "user", .content = "write code"});
  ChatCompletionResponse resp;

  Status st = router_->Complete(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp.choices[0].message.content, "DeepSeek response");
  EXPECT_EQ(deepseek_client_->CallCount(), 1u);
  EXPECT_EQ(ollama_client_->CallCount(), 0u);
  EXPECT_EQ(default_client_->CallCount(), 0u);
}

TEST_F(ModelRouterTest, WildcardPrefixMatchingAndModelRemapping) {
  ollama_client_->SetDefaultResponseText("Ollama response");

  ChatCompletionRequest req;
  req.model = "ollama/phi3";
  req.messages.push_back(ChatMessage{.role = "user", .content = "explain gravity"});
  ChatCompletionResponse resp;

  Status st = router_->Complete(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp.choices[0].message.content, "Ollama response");
  EXPECT_EQ(ollama_client_->CallCount(), 1u);
  EXPECT_EQ(deepseek_client_->CallCount(), 0u);
  EXPECT_EQ(default_client_->CallCount(), 0u);
}

TEST_F(ModelRouterTest, DefaultCascadeFallback) {
  default_client_->SetDefaultResponseText("Default response");

  ChatCompletionRequest req;
  req.model = "unknown-model";
  req.messages.push_back(ChatMessage{.role = "user", .content = "hello"});
  ChatCompletionResponse resp;

  Status st = router_->Complete(req, &resp);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(resp.choices[0].message.content, "Default response");
  EXPECT_EQ(default_client_->CallCount(), 1u);
  EXPECT_EQ(deepseek_client_->CallCount(), 0u);
  EXPECT_EQ(ollama_client_->CallCount(), 0u);
}

TEST_F(ModelRouterTest, UnknownModelWithoutDefaultCascadeReturnsError) {
  ModelRouter strict_router; // No default cascade
  std::vector<CascadeTarget> targets = {
      CascadeTarget{.client = deepseek_client_, .breaker = deepseek_breaker_, .target_model = "deepseek-chat", .priority = 0}};
  strict_router.RegisterRoute("deepseek-chat", targets);

  ChatCompletionRequest req;
  req.model = "non-existent-model";
  req.messages.push_back(ChatMessage{.role = "user", .content = "test"});
  ChatCompletionResponse resp;

  Status st = strict_router.Complete(req, &resp);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code, StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(st.message.find("Unknown or unmapped model identifier"), std::string::npos);
  EXPECT_NE(st.message.find("deepseek-chat"), std::string::npos);
}

TEST_F(ModelRouterTest, CycleDetectionInModelAliases) {
  std::unordered_map<std::string, std::string> acyclic = {
      {"modelA", "modelB"},
      {"modelB", "modelC"},
  };
  std::string err;
  EXPECT_FALSE(ModelRouter::DetectCycle(acyclic, &err));

  std::unordered_map<std::string, std::string> cyclic = {
      {"modelA", "modelB"},
      {"modelB", "modelC"},
      {"modelC", "modelA"},
  };
  EXPECT_TRUE(ModelRouter::DetectCycle(cyclic, &err));
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("Cycle detected"), std::string::npos);
}

TEST_F(ModelRouterTest, CreateFromRegistryAutoConfiguration) {
  ProviderRegistry registry;
  ProviderConfig cfg;
  cfg.name = "ollama";
  cfg.endpoint = "http://localhost:11434/v1/chat/completions";
  cfg.default_model = "llama3.2";
  cfg.supported_models = {"llama3.2", "phi3"};
  registry.RegisterProvider(cfg);

  auto router = ModelRouter::CreateFromRegistry(registry);
  ASSERT_NE(router, nullptr);
  EXPECT_GT(router->RouteCount(), 0u);

  std::string remapped;
  auto cascade = router->ResolveCascade("ollama/phi3", &remapped);
  ASSERT_NE(cascade, nullptr);
  EXPECT_EQ(remapped, "phi3");
}

TEST_F(ModelRouterTest, CreateFromRegistryPropagatesIsolatedRateLimiterToRoutes) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pcfg;
  auto pool = std::make_shared<RedisConnectionPool>(pcfg, mock_redis);
  auto limiter = std::make_shared<DistributedRateLimiter>(pool);

  ProviderRegistry registry;
  registry.SetRateLimiter(limiter);
  registry.SetClientFactory([](const ProviderConfig& cfg) {
    return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
  });

  // 1) Tier 1: ollama (limit = 1 RPM)
  ProviderConfig p_ollama;
  p_ollama.name = "ollama";
  p_ollama.endpoint = "http://localhost:11434/v1/chat/completions";
  p_ollama.default_model = "llama3.2";
  p_ollama.rate_limits.push_back(RateLimitRule{
      .provider = "ollama", .window_name = "minute", .capacity = 1.0, .refill_rate_per_sec = 0.0, .ttl_seconds = 60});
  ASSERT_TRUE(registry.RegisterProvider(p_ollama));

  // 2) Tier 2: deepseek (limit = 5 RPM)
  ProviderConfig p_ds;
  p_ds.name = "deepseek";
  p_ds.endpoint = "https://api.deepseek.com/v1/chat/completions";
  p_ds.api_key = "sk-test";
  p_ds.default_model = "deepseek-chat";
  p_ds.rate_limits.push_back(RateLimitRule{
      .provider = "deepseek", .window_name = "minute", .capacity = 5.0, .refill_rate_per_sec = 0.0, .ttl_seconds = 60});
  ASSERT_TRUE(registry.RegisterProvider(p_ds));

  auto router = ModelRouter::CreateFromRegistry(registry, limiter);

  // Explicit single-provider route ("llama3.2"): 1st call succeeds on ollama, 2nd hits RESOURCE_EXHAUSTED
  ChatCompletionRequest req_ollama;
  req_ollama.model = "llama3.2";
  req_ollama.messages.push_back(ChatMessage{.role = "user", .content = "hi"});
  ChatCompletionResponse resp_o1;
  ASSERT_TRUE(router->Complete(req_ollama, &resp_o1).ok());

  ChatCompletionResponse resp_o2;
  Status st_o2 = router->Complete(req_ollama, &resp_o2);
  EXPECT_FALSE(st_o2.ok());
  EXPECT_EQ(st_o2.code, StatusCode::RESOURCE_EXHAUSTED);

  // Unmapped / generic default cascade route automatically fails over from exhausted ollama -> deepseek!
  ChatCompletionRequest req_default;
  req_default.model = "general-purpose";
  req_default.messages.push_back(ChatMessage{.role = "user", .content = "hi fallback"});
  ChatCompletionResponse resp_def;
  Status st_def = router->Complete(req_default, &resp_def);
  ASSERT_TRUE(st_def.ok());
  EXPECT_EQ(resp_def.model, "deepseek-chat");
}
