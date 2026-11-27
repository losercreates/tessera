#include <gtest/gtest.h>
#include "tessera/backend/mock_backend.hpp"
#include "tessera/backend/mock_upstream_client.hpp"
#include "tessera/cache/cached_backend.hpp"
#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/cache/redis_client.hpp"
#include "tessera/cache/redis_pool.hpp"
#include "tessera/cache/semantic_cache.hpp"
#include "tessera/cache/two_tier_cache.hpp"
#include "tessera/cache/vector_store.hpp"
#include "tessera/common/json.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/routing/model_router.hpp"
#include "tessera/routing/provider_registry.hpp"
#include "tessera/server/gateway_server.hpp"
#include "tessera/server/rpc_client.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace tessera;

namespace {

int ConnectLoopbackRaw(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool SendAllRaw(int fd, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

std::string RecvAllRaw(int fd) {
  std::string out;
  char buf[2048];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

struct ScopedFd {
  int fd{-1};
  explicit ScopedFd(int f) : fd(f) {}
  ~ScopedFd() {
    if (fd >= 0) ::close(fd);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
};

class LiveIngressIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TelemetryRegistry::Instance().Reset();

    mock_redis_ = std::make_shared<MockRedisClient>();
    RedisPoolConfig pool_cfg;
    pool_cfg.min_connections = 2;
    pool_cfg.max_connections = 8;
    redis_pool_ = std::make_shared<RedisConnectionPool>(pool_cfg, mock_redis_);

    auto exact_cache = std::make_shared<ExactCacheService>(redis_pool_, 3600);
    auto emb = std::make_shared<MockEmbeddingEngine>();
    auto vec = std::make_shared<MockVectorIndex>();
    auto sem_cache = std::make_shared<SemanticCacheService>(emb, vec, 0.95f, 3600);
    two_tier_cache_ = std::make_shared<TwoTierCacheService>(exact_cache, sem_cache);

    rate_limiter_ = std::make_shared<DistributedRateLimiter>(redis_pool_);

    ProviderRegistry registry;
    registry.SetRateLimiter(rate_limiter_);
    registry.SetClientFactory([](const ProviderConfig& cfg) {
      auto client = std::make_shared<MockUpstreamClient>(cfg.name, cfg.default_model, cfg.endpoint);
      client->SetDefaultResponseText("Live reply from " + cfg.name + " covering multi-chunk token output cleanly.");
      return client;
    });

    ProviderConfig p1;
    p1.name = "ollama";
    p1.endpoint = "http://localhost:11434/v1/chat/completions";
    p1.default_model = "llama3.2";
    p1.supported_models = {"llama3.2", "phi3"};
    p1.rate_limits.push_back(RateLimitRule{
        .provider = "ollama",
        .window_name = "minute",
        .capacity = 2.0,
        .refill_rate_per_sec = 0.0,
        .ttl_seconds = 60,
    });
    ASSERT_TRUE(registry.RegisterProvider(p1));
    ollama_client_ = std::dynamic_pointer_cast<MockUpstreamClient>(registry.GetProviderClient("ollama"));

    model_router_ = ModelRouter::CreateFromRegistry(registry, rate_limiter_);
    pipeline_ = std::make_shared<CachedCompletionBackend>(model_router_, two_tier_cache_);
  }

  void TearDown() override {
    TelemetryRegistry::Instance().Reset();
  }

  std::shared_ptr<MockRedisClient> mock_redis_;
  std::shared_ptr<RedisConnectionPool> redis_pool_;
  std::shared_ptr<TwoTierCacheService> two_tier_cache_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
  std::shared_ptr<MockUpstreamClient> ollama_client_;
  std::shared_ptr<ModelRouter> model_router_;
  std::shared_ptr<CachedCompletionBackend> pipeline_;
};

TEST_F(LiveIngressIntegrationTest, RemoteRpcLoopbackHealthCompletionAndNoCache) {
  ServerConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.http_port = 0;
  cfg.metrics_port = 0;
  cfg.worker_threads = 2;

  GatewayServer server(cfg, pipeline_);
  ASSERT_TRUE(server.Start());
  uint16_t rpc_port = server.BoundPort();
  ASSERT_GT(rpc_port, 0u);

  RpcClient client("127.0.0.1", rpc_port, 3000);
  HealthStatus hs = HealthStatus::UNKNOWN;
  ASSERT_TRUE(client.CheckHealth(&hs).ok());
  EXPECT_EQ(hs, HealthStatus::SERVING);

  ChatCompletionRequest req;
  req.model = "llama3.2";
  req.correlation_id = "rpc-live-1";
  req.messages.push_back(ChatMessage{.role = "user", .content = "Hello RPC ingress"});

  ChatCompletionResponse resp1;
  Status st1 = client.Complete(req, &resp1);
  ASSERT_TRUE(st1.ok());
  EXPECT_FALSE(resp1.cache_hit);
  EXPECT_EQ(resp1.correlation_id, "rpc-live-1");
  ASSERT_FALSE(resp1.choices.empty());
  EXPECT_NE(resp1.choices[0].message.content.find("Live reply from ollama"), std::string::npos);
  EXPECT_EQ(ollama_client_->CallCount(), 1u);

  // Replay hits cache (consumes 0 quota; no upstream call)
  req.correlation_id = "rpc-live-2";
  ChatCompletionResponse resp2;
  ASSERT_TRUE(client.Complete(req, &resp2).ok());
  EXPECT_TRUE(resp2.cache_hit);
  EXPECT_EQ(resp2.cache_tier, "exact");
  EXPECT_EQ(ollama_client_->CallCount(), 1u);

  // Explicit no_cache bypass consumes remaining 2nd quota token
  req.correlation_id = "rpc-live-bypass";
  req.no_cache = true;
  ChatCompletionResponse resp3;
  ASSERT_TRUE(client.Complete(req, &resp3).ok());
  EXPECT_FALSE(resp3.cache_hit);
  EXPECT_EQ(ollama_client_->CallCount(), 2u);

  server.Stop();

  // Remote client fails cleanly once server stops
  HealthStatus after_hs = HealthStatus::SERVING;
  EXPECT_FALSE(client.CheckHealth(&after_hs).ok());
}

TEST_F(LiveIngressIntegrationTest, OpenAIRestModelsHealthCompletionsSseAndMetrics) {
  ServerConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.http_port = 0;
  cfg.metrics_port = 0;
  cfg.worker_threads = 4;

  GatewayServer server(cfg, pipeline_);
  server.SetModelListProvider([this]() {
    std::vector<ModelInfo> out;
    for (const auto& id : model_router_->GetRegisteredModels()) {
      out.push_back(ModelInfo{.id = id, .owned_by = "tessera", .created = 1700000000});
    }
    return out;
  });
  ASSERT_TRUE(server.Start());

  const uint16_t http_port = server.BoundHttpPort();
  const uint16_t metrics_port = server.BoundMetricsPort();
  ASSERT_GT(http_port, 0u);
  ASSERT_GT(metrics_port, 0u);
  ASSERT_NE(http_port, metrics_port);

  // 1. Liveness/readiness probe
  HttpClientResponse health_resp;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", http_port, "GET", "/health", "", {}, &health_resp).ok());
  EXPECT_EQ(health_resp.status_code, 200);
  EXPECT_NE(health_resp.body.find("SERVING"), std::string::npos);

  // 2. OpenAI model discovery
  HttpClientResponse models_resp;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", http_port, "GET", "/v1/models", "", {}, &models_resp).ok());
  EXPECT_EQ(models_resp.status_code, 200);
  JsonValue models_json;
  ASSERT_TRUE(JsonValue::Parse(models_resp.body, &models_json));
  EXPECT_EQ(models_json.Find("object")->AsString(), "list");
  const JsonValue* data = models_json.Find("data");
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(data->IsArray());
  bool found_llama = false;
  for (const auto& entry : data->AsArray()) {
    if (entry.Find("id") && entry.Find("id")->AsString() == "llama3.2") {
      found_llama = true;
    }
  }
  EXPECT_TRUE(found_llama);

  // 3. OpenAI non-streaming completion -> MISS then HIT-EXACT then Header Cache-Control bypass
  const std::string chat_body =
      R"({"model":"llama3.2","messages":[{"role":"user","content":"Summarize two-tier cache behavior"}]})";
  HttpClientResponse c1;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1",
      http_port,
      "POST",
      "/v1/chat/completions",
      chat_body,
      {{"Content-Type", "application/json"}, {"X-Correlation-ID", "http-cid-1"}},
      &c1)
      .ok());
  EXPECT_EQ(c1.status_code, 200);
  EXPECT_EQ(c1.Header("x-tessera-cache-hit"), "false");
  EXPECT_EQ(c1.Header("x-correlation-id"), "http-cid-1");
  JsonValue c1_json;
  ASSERT_TRUE(JsonValue::Parse(c1.body, &c1_json));
  EXPECT_EQ(c1_json.Find("object")->AsString(), "chat.completion");
  const std::string full_text =
      c1_json.Find("choices")->AsArray()[0].Find("message")->Find("content")->AsString();
  EXPECT_NE(full_text.find("Live reply from ollama"), std::string::npos);
  EXPECT_EQ(ollama_client_->CallCount(), 1u);

  // Exact cache hit (0 upstream calls)
  HttpClientResponse c2;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1",
      http_port,
      "POST",
      "/v1/chat/completions",
      chat_body,
      {{"Content-Type", "application/json"}},
      &c2)
      .ok());
  EXPECT_EQ(c2.status_code, 200);
  EXPECT_EQ(c2.Header("x-tessera-cache-hit"), "true");
  EXPECT_EQ(c2.Header("x-tessera-cache-tier"), "exact");
  EXPECT_EQ(ollama_client_->CallCount(), 1u);

  // 4. OpenAI Server-Sent Events streaming ("stream": true) reassembles cleanly
  const std::string sse_body =
      R"({"model":"llama3.2","stream":true,"messages":[{"role":"user","content":"Summarize two-tier cache behavior"}]})";
  std::vector<std::string> sse_events;
  HttpClientResponse sse_resp;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1",
      http_port,
      "POST",
      "/v1/chat/completions",
      sse_body,
      {{"Content-Type", "application/json"}},
      &sse_resp,
      5000,
      [&sse_events](std::string_view ev) { sse_events.emplace_back(ev); })
      .ok());
  EXPECT_EQ(sse_resp.status_code, 200);
  EXPECT_NE(sse_resp.Header("content-type").find("text/event-stream"), std::string::npos);
  ASSERT_GE(sse_events.size(), 3u);
  EXPECT_EQ(sse_events.back(), "[DONE]");

  std::string reassembled;
  bool saw_stop_finish = false;
  for (size_t i = 0; i + 1 < sse_events.size(); ++i) {
    JsonValue chunk;
    ASSERT_TRUE(JsonValue::Parse(sse_events[i], &chunk));
    EXPECT_EQ(chunk.Find("object")->AsString(), "chat.completion.chunk");
    const JsonValue& ch0 = chunk.Find("choices")->AsArray()[0];
    if (const JsonValue* delta = ch0.Find("delta")) {
      if (const JsonValue* content = delta->Find("content"); content && content->IsString()) {
        reassembled += content->AsString();
      }
    }
    if (const JsonValue* fin = ch0.Find("finish_reason"); fin && fin->IsString() && fin->AsString() == "stop") {
      saw_stop_finish = true;
    }
  }
  EXPECT_EQ(reassembled, full_text);
  EXPECT_TRUE(saw_stop_finish);

  // 5. HTTP Cache-Control: no-cache bypass consumes 2nd quota token, then 3rd uncached hits 429 + Retry-After
  HttpClientResponse c3;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1",
      http_port,
      "POST",
      "/v1/chat/completions",
      chat_body,
      {{"Content-Type", "application/json"}, {"Cache-Control", "no-cache"}},
      &c3)
      .ok());
  EXPECT_EQ(c3.status_code, 200);
  EXPECT_EQ(c3.Header("x-tessera-cache-hit"), "false");
  EXPECT_EQ(ollama_client_->CallCount(), 2u);

  const std::string third_prompt =
      R"({"model":"llama3.2","no_cache":true,"messages":[{"role":"user","content":"Brand new uncached prompt exhausting quota"}]})";
  HttpClientResponse rate_limited;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1",
      http_port,
      "POST",
      "/v1/chat/completions",
      third_prompt,
      {{"Content-Type", "application/json"}},
      &rate_limited)
      .ok());
  EXPECT_EQ(rate_limited.status_code, 429);
  EXPECT_FALSE(rate_limited.Header("retry-after").empty());

  // 6. Malformed JSON / wrong route / wrong method error handling
  HttpClientResponse bad_json;
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1", http_port, "POST", "/v1/chat/completions", "{broken_json", {}, &bad_json)
      .ok());
  EXPECT_EQ(bad_json.status_code, 400);

  HttpClientResponse not_found;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", http_port, "GET", "/does/not/exist", "", {}, &not_found).ok());
  EXPECT_EQ(not_found.status_code, 404);

  HttpClientResponse wrong_method;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", http_port, "GET", "/v1/chat/completions", "", {}, &wrong_method).ok());
  EXPECT_EQ(wrong_method.status_code, 405);

  // 7. Prometheus exposition on dedicated METRICS_PORT (not on ingress HTTP port unless configured)
  HttpClientResponse metrics_on_ingress;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", http_port, "GET", "/metrics", "", {}, &metrics_on_ingress).ok());
  EXPECT_EQ(metrics_on_ingress.status_code, 404);

  HttpClientResponse metrics_resp;
  ASSERT_TRUE(HttpRequestSync("127.0.0.1", metrics_port, "GET", "/metrics", "", {}, &metrics_resp).ok());
  EXPECT_EQ(metrics_resp.status_code, 200);
  EXPECT_NE(metrics_resp.Header("content-type").find("text/plain"), std::string::npos);
  EXPECT_NE(metrics_resp.body.find("# TYPE gateway_requests_total counter"), std::string::npos);
  EXPECT_NE(metrics_resp.body.find("gateway_cache_hits_total"), std::string::npos);

  server.Stop();
}

TEST_F(LiveIngressIntegrationTest, GracefulShutdownDrainsHttpSseRpcAndRejectsPostDrain) {
  rate_limiter_->ClearRules("ollama");
  rate_limiter_->AddRule(RateLimitRule{
      .provider = "ollama",
      .window_name = "minute",
      .capacity = 10.0,
      .refill_rate_per_sec = 0.0,
      .ttl_seconds = 60,
  });
  ollama_client_->SetSimulatedLatency(std::chrono::milliseconds(160));

  ServerConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.http_port = 0;
  cfg.metrics_port = 0;
  cfg.worker_threads = 4;
  cfg.graceful_drain_timeout_ms = 2000;

  GatewayServer server(cfg, pipeline_);
  ASSERT_TRUE(server.Start());

  const uint16_t rpc_port = server.BoundPort();
  const uint16_t http_port = server.BoundHttpPort();

  std::atomic<int> http_status{0};
  std::atomic<int> sse_status{0};
  std::atomic<bool> rpc_ok{false};

  std::thread t_http([&]() {
    HttpClientResponse resp;
    const std::string body =
        R"({"model":"llama3.2","messages":[{"role":"user","content":"Drain HTTP test one"}]})";
    if (HttpRequestSync("127.0.0.1", http_port, "POST", "/v1/chat/completions", body, {}, &resp).ok()) {
      http_status.store(resp.status_code);
    }
  });

  std::thread t_sse([&]() {
    HttpClientResponse resp;
    const std::string body =
        R"({"model":"llama3.2","stream":true,"messages":[{"role":"user","content":"Drain SSE test two"}]})";
    if (HttpRequestSync("127.0.0.1", http_port, "POST", "/v1/chat/completions", body, {}, &resp).ok()) {
      sse_status.store(resp.status_code);
    }
  });

  std::thread t_rpc([&]() {
    RpcClient client("127.0.0.1", rpc_port, 5000);
    ChatCompletionRequest req;
    req.model = "llama3.2";
    req.no_cache = true;
    req.messages.push_back(ChatMessage{.role = "user", .content = "Drain RPC test three"});
    ChatCompletionResponse resp;
    if (client.Complete(req, &resp).ok()) {
      rpc_ok.store(true);
    }
  });

  // Wait for active requests to reach backend
  for (int i = 0; i < 40 && server.InFlightCount() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::thread stopper([&]() { server.Stop(); });

  // Wait until drain begins (health becomes NOT_SERVING) while upstream requests are still completing
  for (int i = 0; i < 40 && server.CheckHealth() == HealthStatus::SERVING; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(server.CheckHealth(), HealthStatus::NOT_SERVING);

  HttpClientResponse drain_health;
  if (HttpRequestSync("127.0.0.1", http_port, "GET", "/health", "", {}, &drain_health, 1000).ok()) {
    EXPECT_EQ(drain_health.status_code, 503);
    EXPECT_NE(drain_health.body.find("NOT_SERVING"), std::string::npos);
  }

  HttpClientResponse post_drain;
  if (HttpRequestSync(
          "127.0.0.1",
          http_port,
          "POST",
          "/v1/chat/completions",
          R"({"model":"llama3.2","messages":[{"role":"user","content":"Rejected during drain"}]})",
          {},
          &post_drain,
          1000)
          .ok()) {
    EXPECT_EQ(post_drain.status_code, 503);
  }

  stopper.join();
  t_http.join();
  t_sse.join();
  t_rpc.join();

  EXPECT_EQ(http_status.load(), 200);
  EXPECT_EQ(sse_status.load(), 200);
  EXPECT_TRUE(rpc_ok.load());
  EXPECT_EQ(ollama_client_->CallCount(), 3u);
  EXPECT_EQ(server.InFlightCount(), 0);
}

TEST_F(LiveIngressIntegrationTest, BindConflictRollsBackPartiallyStartedListeners) {
  int busy_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(busy_fd, 0);
  ScopedFd guard(busy_fd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  ASSERT_EQ(::bind(busy_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(busy_fd, 4), 0);

  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(busy_fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
  const uint16_t busy_port = ntohs(addr.sin_port);
  ASSERT_GT(busy_port, 0u);

  // HTTP bind failure must tear down already-started RPC listener
  ServerConfig cfg1;
  cfg1.host = "127.0.0.1";
  cfg1.port = 0;
  cfg1.http_port = busy_port;
  cfg1.metrics_port = 0;
  GatewayServer s1(cfg1, pipeline_);
  EXPECT_FALSE(s1.Start());
  EXPECT_FALSE(s1.IsRunning());
  EXPECT_EQ(s1.BoundPort(), 0u);
  EXPECT_EQ(s1.BoundHttpPort(), 0u);

  // Metrics bind failure must tear down both RPC and HTTP listeners
  ServerConfig cfg2;
  cfg2.host = "127.0.0.1";
  cfg2.port = 0;
  cfg2.http_port = 0;
  cfg2.metrics_port = busy_port;
  GatewayServer s2(cfg2, pipeline_);
  EXPECT_FALSE(s2.Start());
  EXPECT_FALSE(s2.IsRunning());
  EXPECT_EQ(s2.BoundPort(), 0u);
  EXPECT_EQ(s2.BoundHttpPort(), 0u);
  EXPECT_EQ(s2.BoundMetricsPort(), 0u);
}

TEST_F(LiveIngressIntegrationTest, OversizedBodyFragmentedTcpTruncatedFramesAndEarlySseClose) {
  ServerConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.http_port = 0;
  cfg.metrics_port = 0;
  cfg.worker_threads = 2;
  cfg.max_message_size = 1024;

  GatewayServer server(cfg, pipeline_);
  ASSERT_TRUE(server.Start());

  const uint16_t rpc_port = server.BoundPort();
  const uint16_t http_port = server.BoundHttpPort();

  // 1. Oversized HTTP Content-Length returns 413 cleanly
  HttpClientResponse oversized;
  std::string big_body(2048, 'x');
  ASSERT_TRUE(HttpRequestSync(
      "127.0.0.1", http_port, "POST", "/v1/chat/completions", big_body, {}, &oversized)
      .ok());
  EXPECT_EQ(oversized.status_code, 413);

  // 2. Fragmented HTTP request delivered in tiny chunks still parses cleanly
  {
    int fd = ConnectLoopbackRaw(http_port);
    ASSERT_GE(fd, 0);
    ScopedFd guard(fd);
    const std::string frag = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    for (size_t i = 0; i < frag.size(); i += 5) {
      size_t n = std::min<size_t>(5, frag.size() - i);
      ASSERT_TRUE(SendAllRaw(fd, std::string_view(frag).substr(i, n)));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string raw_resp = RecvAllRaw(fd);
    EXPECT_NE(raw_resp.find("200 OK"), std::string::npos);
    EXPECT_NE(raw_resp.find("SERVING"), std::string::npos);
  }

  // 3. Premature client disconnect during SSE streaming causes zero crash/SIGPIPE
  {
    ollama_client_->SetSimulatedLatency(std::chrono::milliseconds(50));
    int fd = ConnectLoopbackRaw(http_port);
    ASSERT_GE(fd, 0);
    std::string body = R"({"model":"llama3.2","stream":true,"messages":[{"role":"user","content":"Abort early SSE"}]})";
    std::ostringstream oss;
    oss << "POST /v1/chat/completions HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    ASSERT_TRUE(SendAllRaw(fd, oss.str()));
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ollama_client_->SetSimulatedLatency(std::chrono::milliseconds(0));
  }

  // 4. Truncated & invalid RPC frame does not crash server
  {
    int fd = ConnectLoopbackRaw(rpc_port);
    ASSERT_GE(fd, 0);
    const uint8_t bogus[4] = {0xff, 0xff, 0xff, 0xff};
    SendAllRaw(fd, std::string_view(reinterpret_cast<const char*>(bogus), 4));
    ::close(fd);
  }

  // Server remains completely healthy afterward
  RpcClient rpc("127.0.0.1", rpc_port, 2000);
  HealthStatus hs = HealthStatus::UNKNOWN;
  ASSERT_TRUE(rpc.CheckHealth(&hs).ok());
  EXPECT_EQ(hs, HealthStatus::SERVING);

  server.Stop();
}

}  // namespace
