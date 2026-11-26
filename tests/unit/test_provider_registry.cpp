#include <gtest/gtest.h>
#include "tessera/routing/provider_registry.hpp"
#include "tessera/routing/model_router.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <cstdlib>

using namespace tessera;

class ProviderRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();
  }
};

TEST_F(ProviderRegistryTest, RegisterAndLookupProviders) {
  ProviderRegistry registry;
  EXPECT_EQ(registry.ProviderCount(), 0u);

  ProviderConfig cfg;
  cfg.name = "ollama";
  cfg.endpoint = "http://localhost:11434/v1/chat/completions";
  cfg.default_model = "llama3.2";
  cfg.supported_models = {"llama3.2", "phi3"};

  std::string err;
  EXPECT_TRUE(registry.RegisterProvider(cfg, &err));
  EXPECT_TRUE(err.empty());
  EXPECT_EQ(registry.ProviderCount(), 1u);
  EXPECT_TRUE(registry.HasProvider("ollama"));
  EXPECT_FALSE(registry.HasProvider("non-existent"));

  // Lookup client and breaker
  auto client = registry.GetProviderClient("ollama");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->ProviderName(), "ollama");
  EXPECT_EQ(client->Endpoint(), "http://localhost:11434/v1/chat/completions");
  EXPECT_EQ(client->DefaultModel(), "llama3.2");

  auto breaker = registry.GetProviderBreaker("ollama");
  ASSERT_NE(breaker, nullptr);
  EXPECT_TRUE(breaker->CanAttempt());

  const auto* stored_cfg = registry.GetProviderConfig("ollama");
  ASSERT_NE(stored_cfg, nullptr);
  EXPECT_TRUE(stored_cfg->IsLocal());

  // Unregister
  EXPECT_TRUE(registry.UnregisterProvider("ollama"));
  EXPECT_EQ(registry.ProviderCount(), 0u);
  EXPECT_FALSE(registry.HasProvider("ollama"));
}

TEST_F(ProviderRegistryTest, RejectsInvalidProviderConfiguration) {
  ProviderRegistry registry;

  ProviderConfig cfg;
  cfg.name = ""; // Empty name
  cfg.endpoint = "http://localhost:11434";
  cfg.default_model = "m1";

  std::string err;
  EXPECT_FALSE(registry.RegisterProvider(cfg, &err));
  EXPECT_FALSE(err.empty());

  // Bad endpoint scheme
  cfg.name = "test";
  cfg.endpoint = "ftp://localhost:11434";
  EXPECT_FALSE(registry.RegisterProvider(cfg, &err));

  // Empty model
  cfg.endpoint = "http://localhost:11434";
  cfg.default_model = "";
  EXPECT_FALSE(registry.RegisterProvider(cfg, &err));
}

TEST_F(ProviderRegistryTest, CustomFactoriesForHermeticMocking) {
  ProviderRegistry registry;

  bool client_factory_called = false;
  registry.SetClientFactory([&](const ProviderConfig& cfg) {
    client_factory_called = true;
    return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
  });

  ProviderConfig cfg;
  cfg.name = "mock_provider";
  cfg.endpoint = "https://mock.ai/v1/chat/completions";
  cfg.default_model = "mock-1";

  EXPECT_TRUE(registry.RegisterProvider(cfg));
  EXPECT_TRUE(client_factory_called);

  auto client = registry.GetProviderClient("mock_provider");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->ProviderName(), "mock_provider");
}

TEST_F(ProviderRegistryTest, DefaultConfigsAreValid) {
  auto groq = ProviderRegistry::DefaultGroqConfig();
  std::string err;
  EXPECT_TRUE(groq.Validate(&err));
  EXPECT_EQ(groq.name, "groq");

  auto openrouter = ProviderRegistry::DefaultOpenRouterConfig();
  EXPECT_TRUE(openrouter.Validate(&err));
  EXPECT_EQ(openrouter.name, "openrouter");
}

TEST_F(ProviderRegistryTest, LoadFromEnvironmentDefaults) {
  // Clear any overrides for test isolation
  unsetenv("TESSERA_PROVIDERS");

  auto registry = ProviderRegistry::LoadFromEnvironment(true);
  EXPECT_GE(registry.ProviderCount(), 2u);
  EXPECT_TRUE(registry.HasProvider("groq"));
  EXPECT_TRUE(registry.HasProvider("openrouter"));

  auto providers = registry.GetRegisteredProviders();
  EXPECT_GE(providers.size(), 2u);
}

TEST_F(ProviderRegistryTest, LoadFromEnvironmentCustomProviders) {
  setenv("TESSERA_PROVIDERS", "ollama,deepseek", 1);
  setenv("PROVIDER_OLLAMA_ENDPOINT", "http://localhost:11434/v1/chat/completions", 1);
  setenv("PROVIDER_OLLAMA_MODEL", "llama3.2", 1);
  setenv("PROVIDER_DEEPSEEK_ENDPOINT", "https://api.deepseek.com/v1/chat/completions", 1);
  setenv("PROVIDER_DEEPSEEK_KEY", "sk-deepseek-test", 1);
  setenv("PROVIDER_DEEPSEEK_MODEL", "deepseek-chat", 1);
  setenv("PROVIDER_DEEPSEEK_RPM", "60", 1);

  auto registry = ProviderRegistry::LoadFromEnvironment(false);
  EXPECT_EQ(registry.ProviderCount(), 2u);
  EXPECT_TRUE(registry.HasProvider("ollama"));
  EXPECT_TRUE(registry.HasProvider("deepseek"));

  const auto* ollama_cfg = registry.GetProviderConfig("ollama");
  ASSERT_NE(ollama_cfg, nullptr);
  EXPECT_EQ(ollama_cfg->endpoint, "http://localhost:11434/v1/chat/completions");
  EXPECT_EQ(ollama_cfg->default_model, "llama3.2");
  EXPECT_TRUE(ollama_cfg->IsLocal());

  const auto* deepseek_cfg = registry.GetProviderConfig("deepseek");
  ASSERT_NE(deepseek_cfg, nullptr);
  EXPECT_EQ(deepseek_cfg->endpoint, "https://api.deepseek.com/v1/chat/completions");
  EXPECT_EQ(deepseek_cfg->api_key, "sk-deepseek-test");
  EXPECT_EQ(deepseek_cfg->default_model, "deepseek-chat");
  EXPECT_FALSE(deepseek_cfg->IsLocal());
  ASSERT_FALSE(deepseek_cfg->rate_limits.empty());
  EXPECT_EQ(deepseek_cfg->rate_limits[0].capacity, 60.0);

  // Clean up env
  unsetenv("TESSERA_PROVIDERS");
  unsetenv("PROVIDER_OLLAMA_ENDPOINT");
  unsetenv("PROVIDER_OLLAMA_MODEL");
  unsetenv("PROVIDER_DEEPSEEK_ENDPOINT");
  unsetenv("PROVIDER_DEEPSEEK_KEY");
  unsetenv("PROVIDER_DEEPSEEK_MODEL");
  unsetenv("PROVIDER_DEEPSEEK_RPM");
}

TEST_F(ProviderRegistryTest, LoadFromJsonSpecification) {
  std::string json_input = R"({
    "providers": [
      {
        "name": "ollama",
        "endpoint": "http://localhost:11434/v1/chat/completions",
        "default_model": "llama3.2"
      },
      {
        "name": "together",
        "endpoint": "https://api.together.xyz/v1/chat/completions",
        "api_key": "tok-together-123",
        "default_model": "mistralai/Mixtral-8x7B"
      }
    ]
  })";

  auto registry = ProviderRegistry::LoadFromJson(json_input);
  EXPECT_EQ(registry.ProviderCount(), 2u);
  EXPECT_TRUE(registry.HasProvider("ollama"));
  EXPECT_TRUE(registry.HasProvider("together"));

  const auto* together = registry.GetProviderConfig("together");
  ASSERT_NE(together, nullptr);
  EXPECT_EQ(together->api_key, "tok-together-123");
  EXPECT_EQ(together->default_model, "mistralai/Mixtral-8x7B");
}

TEST_F(ProviderRegistryTest, LoadFromIndexedEnvironmentVariables) {
  // Clear any existing env
  unsetenv("TESSERA_PROVIDERS");
  unsetenv("PROVIDER_1_NAME");
  unsetenv("PROVIDER_1_ENDPOINT");
  unsetenv("PROVIDER_1_MODEL");
  unsetenv("PROVIDER_1_RPM");
  unsetenv("PROVIDER_2_NAME");
  unsetenv("PROVIDER_2_ENDPOINT");
  unsetenv("PROVIDER_2_KEY");
  unsetenv("PROVIDER_2_MODEL");
  unsetenv("PROVIDER_2_RPM");
  unsetenv("PROVIDER_3_NAME");
  unsetenv("PROVIDER_3_ENDPOINT");
  unsetenv("PROVIDER_3_KEY");
  unsetenv("PROVIDER_3_MODEL");
  unsetenv("PROVIDER_3_RPM");

  // Configure indexed providers: Upstream 1 (ollama), Upstream 2 (groq), Upstream 3 (deepseek)
  setenv("PROVIDER_1_NAME", "ollama", 1);
  setenv("PROVIDER_1_ENDPOINT", "http://ollama-service:11434/v1/chat/completions", 1);
  setenv("PROVIDER_1_MODEL", "llama3.2", 1);
  setenv("PROVIDER_1_RPM", "120", 1);

  setenv("PROVIDER_2_NAME", "groq", 1);
  setenv("PROVIDER_2_ENDPOINT", "https://api.groq.com/openai/v1/chat/completions", 1);
  setenv("PROVIDER_2_KEY", "gsk_test123", 1);
  setenv("PROVIDER_2_MODEL", "llama-3.1-8b-instant", 1);
  setenv("PROVIDER_2_RPM", "30", 1);

  setenv("PROVIDER_3_NAME", "deepseek", 1);
  setenv("PROVIDER_3_ENDPOINT", "https://api.deepseek.com/v1/chat/completions", 1);
  setenv("PROVIDER_3_KEY", "sk_test456", 1);
  setenv("PROVIDER_3_MODEL", "deepseek-chat", 1);
  setenv("PROVIDER_3_RPM", "60", 1);

  auto registry = ProviderRegistry::LoadFromEnvironment(false);
  EXPECT_EQ(registry.ProviderCount(), 3u);

  // Check exact order preservation: ollama (1) -> groq (2) -> deepseek (3)
  auto registered = registry.GetRegisteredProviders();
  ASSERT_EQ(registered.size(), 3u);
  EXPECT_EQ(registered[0], "ollama");
  EXPECT_EQ(registered[1], "groq");
  EXPECT_EQ(registered[2], "deepseek");

  // Check that openrouter is NOT present because it is NOT mandatory!
  EXPECT_FALSE(registry.HasProvider("openrouter"));

  const auto* p1 = registry.GetProviderConfig("ollama");
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(p1->endpoint, "http://ollama-service:11434/v1/chat/completions");
  EXPECT_EQ(p1->default_model, "llama3.2");
  ASSERT_FALSE(p1->rate_limits.empty());
  EXPECT_EQ(p1->rate_limits[0].capacity, 120.0);

  const auto* p2 = registry.GetProviderConfig("groq");
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->endpoint, "https://api.groq.com/openai/v1/chat/completions");
  EXPECT_EQ(p2->api_key, "gsk_test123");
  EXPECT_EQ(p2->default_model, "llama-3.1-8b-instant");
  ASSERT_FALSE(p2->rate_limits.empty());
  EXPECT_EQ(p2->rate_limits[0].capacity, 30.0);

  const auto* p3 = registry.GetProviderConfig("deepseek");
  ASSERT_NE(p3, nullptr);
  EXPECT_EQ(p3->endpoint, "https://api.deepseek.com/v1/chat/completions");
  EXPECT_EQ(p3->api_key, "sk_test456");
  EXPECT_EQ(p3->default_model, "deepseek-chat");
  ASSERT_FALSE(p3->rate_limits.empty());
  EXPECT_EQ(p3->rate_limits[0].capacity, 60.0);

  // Also verify ModelRouter::CreateFromRegistry preserves the priority ordering
  auto router = ModelRouter::CreateFromRegistry(registry);
  std::string remapped;
  auto default_cascade = router->ResolveCascade("unmapped-model", &remapped);
  ASSERT_NE(default_cascade, nullptr);
  ASSERT_EQ(default_cascade->Targets().size(), 3u);
  EXPECT_EQ(default_cascade->Targets()[0].client->ProviderName(), "ollama");
  EXPECT_EQ(default_cascade->Targets()[1].client->ProviderName(), "groq");
  EXPECT_EQ(default_cascade->Targets()[2].client->ProviderName(), "deepseek");

  // Clean up
  unsetenv("PROVIDER_1_NAME");
  unsetenv("PROVIDER_1_ENDPOINT");
  unsetenv("PROVIDER_1_MODEL");
  unsetenv("PROVIDER_1_RPM");
  unsetenv("PROVIDER_2_NAME");
  unsetenv("PROVIDER_2_ENDPOINT");
  unsetenv("PROVIDER_2_KEY");
  unsetenv("PROVIDER_2_MODEL");
  unsetenv("PROVIDER_2_RPM");
  unsetenv("PROVIDER_3_NAME");
  unsetenv("PROVIDER_3_ENDPOINT");
  unsetenv("PROVIDER_3_KEY");
  unsetenv("PROVIDER_3_MODEL");
  unsetenv("PROVIDER_3_RPM");
}

TEST_F(ProviderRegistryTest, CatalogExclusiveRateLimiterAndTelemetryParity) {
  auto mock_redis = std::make_shared<MockRedisClient>();
  RedisPoolConfig pool_cfg;
  auto redis_pool = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis);
  auto limiter = std::make_shared<DistributedRateLimiter>(redis_pool);

  EXPECT_FALSE(limiter->HasRules("groq"));
  EXPECT_FALSE(limiter->HasRules("openrouter"));
  EXPECT_TRUE(TelemetryRegistry::Instance().GetTrackedProviders().empty());

  ProviderRegistry registry;
  registry.SetRateLimiter(limiter);
  registry.SetClientFactory([](const ProviderConfig& cfg) {
    return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
  });

  ProviderConfig ollama;
  ollama.name = "ollama";
  ollama.endpoint = "http://localhost:11434/v1/chat/completions";
  ollama.default_model = "llama3.2";
  ollama.rate_limits.push_back(RateLimitRule{
      .provider = "ollama",
      .window_name = "minute",
      .capacity = 12.0,
      .refill_rate_per_sec = 12.0 / 60.0,
      .ttl_seconds = 60,
  });
  ASSERT_TRUE(registry.RegisterProvider(ollama));

  ProviderConfig deepseek;
  deepseek.name = "deepseek";
  deepseek.endpoint = "https://api.deepseek.com/v1/chat/completions";
  deepseek.default_model = "deepseek-chat";
  deepseek.rate_limits.push_back(RateLimitRule{
      .provider = "deepseek",
      .window_name = "minute",
      .capacity = 60.0,
      .refill_rate_per_sec = 60.0 / 60.0,
      .ttl_seconds = 60,
  });
  ASSERT_TRUE(registry.RegisterProvider(deepseek));

  // Verify idempotent rule attachment on repeated registration / re-registration
  registry.RegisterRateLimits(*limiter);
  EXPECT_EQ(limiter->GetRules("ollama").size(), 1u);
  EXPECT_EQ(limiter->GetRules("deepseek").size(), 1u);

  ollama.rate_limits[0].capacity = 24.0;
  ASSERT_TRUE(registry.RegisterProvider(ollama));
  ASSERT_EQ(limiter->GetRules("ollama").size(), 1u);
  EXPECT_EQ(limiter->GetRules("ollama")[0].capacity, 24.0);

  EXPECT_FALSE(limiter->HasRules("groq"));
  EXPECT_FALSE(limiter->HasRules("openrouter"));

  auto tracked = TelemetryRegistry::Instance().GetTrackedProviders();
  EXPECT_EQ(tracked.size(), 2u);
  std::string prom = TelemetryRegistry::Instance().ToPrometheusFormat();
  EXPECT_NE(prom.find("circuit_breaker_state{provider=\"ollama\"} 0"), std::string::npos);
  EXPECT_NE(prom.find("circuit_breaker_state{provider=\"deepseek\"} 0"), std::string::npos);
  EXPECT_EQ(prom.find("provider=\"groq\""), std::string::npos);
  EXPECT_EQ(prom.find("provider=\"openrouter\""), std::string::npos);
}

TEST_F(ProviderRegistryTest, LoadFromEnvironmentCustomClientFactory) {
  setenv("TESSERA_PROVIDERS", "ollama,deepseek", 1);

  size_t factory_invocations = 0;
  auto registry = ProviderRegistry::LoadFromEnvironment(
      false,
      [&factory_invocations](const ProviderConfig& cfg) {
        factory_invocations++;
        return std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
      });

  EXPECT_EQ(factory_invocations, 2u);
  EXPECT_EQ(registry.ProviderCount(), 2u);
  EXPECT_NE(std::dynamic_pointer_cast<MockUpstreamClient>(registry.GetProviderClient("ollama")), nullptr);
  EXPECT_NE(std::dynamic_pointer_cast<MockUpstreamClient>(registry.GetProviderClient("deepseek")), nullptr);

  auto tracked = TelemetryRegistry::Instance().GetTrackedProviders();
  EXPECT_EQ(tracked.size(), 2u);

  unsetenv("TESSERA_PROVIDERS");
}
