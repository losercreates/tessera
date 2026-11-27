#include "tessera/common/json.hpp"
#include "tessera/common/types.hpp"
#include "tessera/server/rpc_client.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog << " [OPTIONS]\n"
            << "Remote CLI client for a running Tessera Gateway server\n\n"
            << "Options:\n"
            << "  --host=<addr>          Server host (default: $GATEWAY_HOST or 127.0.0.1)\n"
            << "  --port=<port>          Server RPC port (default: $GATEWAY_PORT or 50051)\n"
            << "  --http-port=<port>     Server OpenAI HTTP port (default: $HTTP_PORT or 8080)\n"
            << "  --metrics-port=<port>  Server Prometheus HTTP port (default: $METRICS_PORT or 9090)\n"
            << "  --health               Query server health probe and exit 0 iff SERVING\n"
            << "  --metrics              Fetch Prometheus exposition text from GET /metrics\n"
            << "  --message=<text>       Prompt text for completion request\n"
            << "  --model=<id>           Target model identifier (default: \"auto\")\n"
            << "  --no-cache             Bypass cache reads via request flag/header\n"
            << "  --http                 Use OpenAI HTTP REST ingress instead of RPC\n"
            << "  --stream               Use HTTP SSE streaming (implies --http)\n"
            << "  --timeout-ms=<ms>      Remote request timeout in ms (default: 5000)\n"
            << "  --help                 Show this help message\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 50051;
  uint16_t http_port = 8080;
  uint16_t metrics_port = 9090;
  uint32_t timeout_ms = 5000;

  bool check_health = false;
  bool fetch_metrics = false;
  bool has_message = false;
  bool no_cache = false;
  bool use_http = false;
  bool stream = false;
  std::string user_msg = "Hello, gateway!";
  std::string model = "auto";

  if (const char* env_host = std::getenv("GATEWAY_HOST")) {
    if (*env_host != '\0') host = env_host;
  }
  if (const char* env_port = std::getenv("GATEWAY_PORT")) {
    port = static_cast<uint16_t>(std::stoi(env_port));
  }
  if (const char* env_http = std::getenv("HTTP_PORT")) {
    http_port = static_cast<uint16_t>(std::stoi(env_http));
  }
  if (const char* env_metrics = std::getenv("METRICS_PORT")) {
    metrics_port = static_cast<uint16_t>(std::stoi(env_metrics));
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else if (arg == "--health") {
      check_health = true;
    } else if (arg == "--metrics") {
      fetch_metrics = true;
    } else if (arg == "--no-cache" || arg == "--no_cache") {
      no_cache = true;
    } else if (arg == "--http") {
      use_http = true;
    } else if (arg == "--stream") {
      stream = true;
      use_http = true;
    } else if (arg.rfind("--host=", 0) == 0) {
      host = arg.substr(7);
    } else if (arg.rfind("--port=", 0) == 0) {
      port = static_cast<uint16_t>(std::stoi(arg.substr(7)));
    } else if (arg.rfind("--http-port=", 0) == 0) {
      http_port = static_cast<uint16_t>(std::stoi(arg.substr(12)));
    } else if (arg.rfind("--metrics-port=", 0) == 0) {
      metrics_port = static_cast<uint16_t>(std::stoi(arg.substr(15)));
    } else if (arg.rfind("--timeout-ms=", 0) == 0) {
      timeout_ms = static_cast<uint32_t>(std::stoul(arg.substr(13)));
    } else if (arg.rfind("--message=", 0) == 0) {
      user_msg = arg.substr(10);
      has_message = true;
    } else if (arg == "--message" && i + 1 < argc) {
      user_msg = argv[++i];
      has_message = true;
    } else if (arg.rfind("--model=", 0) == 0) {
      model = arg.substr(8);
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (check_health) {
    if (use_http) {
      tessera::HttpClientResponse hresp;
      tessera::Status st = tessera::HttpRequestSync(
          host, http_port, "GET", "/health", "", {}, &hresp, timeout_ms);
      if (!st.ok()) {
        std::cerr << "Health check failed: " << st.message << "\n";
        return 1;
      }
      std::cout << "Health Check (HTTP): " << hresp.status_code << " " << hresp.body << "\n";
      return hresp.status_code == 200 ? 0 : 1;
    }

    tessera::RpcClient client(host, port, timeout_ms);
    tessera::HealthStatus hs = tessera::HealthStatus::UNKNOWN;
    tessera::Status st = client.CheckHealth(&hs);
    if (!st.ok()) {
      std::cerr << "Health check failed: [" << tessera::StatusCodeToString(st.code)
                << "] " << st.message << "\n";
      return 1;
    }
    std::cout << "Health Check: " << tessera::HealthStatusToString(hs) << "\n";
    return hs == tessera::HealthStatus::SERVING ? 0 : 1;
  }

  bool send_completion = has_message || !fetch_metrics;
  if (send_completion) {
    if (use_http) {
      tessera::ChatCompletionRequest req;
      req.model = model;
      req.messages.push_back(tessera::ChatMessage{.role = "user", .content = user_msg});
      req.stream = stream;
      req.no_cache = no_cache;

      std::string json_body = tessera::json_codec::ChatRequestToJson(req).Dump();
      tessera::HttpClientResponse hresp;
      std::string streamed_text;
      tessera::Status st = tessera::HttpRequestSync(
          host,
          http_port,
          "POST",
          "/v1/chat/completions",
          json_body,
          {{"Content-Type", "application/json"}},
          &hresp,
          timeout_ms,
          [&](std::string_view sse_data) {
            if (sse_data == "[DONE]") return;
            tessera::JsonValue chunk;
            if (!tessera::JsonValue::Parse(sse_data, &chunk)) return;
            const auto* choices = chunk.Find("choices");
            if (choices == nullptr || !choices->IsArray() || choices->AsArray().empty()) return;
            const auto* delta = choices->AsArray()[0].Find("delta");
            if (delta == nullptr) return;
            if (const auto* c = delta->Find("content"); c && c->IsString()) {
              streamed_text.append(c->AsString());
            }
          });

      if (!st.ok()) {
        std::cerr << "HTTP request failed: " << st.message << "\n";
        return 1;
      }
      if (hresp.status_code < 200 || hresp.status_code >= 300) {
        std::cerr << "HTTP " << hresp.status_code << ": " << hresp.body << "\n";
        return 1;
      }

      std::cout << "\n--- HTTP ChatCompletion Response ---\n";
      std::cout << "Endpoint:       http://" << host << ":" << http_port << "/v1/chat/completions\n";
      std::cout << "Cache Hit:      " << hresp.Header("x-tessera-cache-hit") << "\n";
      if (!hresp.Header("x-correlation-id").empty()) {
        std::cout << "Correlation ID: " << hresp.Header("x-correlation-id") << "\n";
      }
      if (stream) {
        std::cout << "Streamed Text:  " << streamed_text << "\n";
      } else {
        std::cout << "Payload:        " << hresp.body << "\n";
      }
      std::cout << "-------------------------------------\n";
    } else {
      tessera::RpcClient client(host, port, timeout_ms);
      tessera::ChatCompletionRequest req;
      req.model = model;
      req.messages.push_back(tessera::ChatMessage{.role = "system", .content = "You are a helpful assistant."});
      req.messages.push_back(tessera::ChatMessage{.role = "user", .content = user_msg});
      req.no_cache = no_cache;

      tessera::ChatCompletionResponse resp;
      tessera::Status st = client.Complete(req, &resp);
      if (!st.ok()) {
        std::cerr << "Request failed: [" << tessera::StatusCodeToString(st.code) << "] " << st.message << "\n";
        return 1;
      }

      std::cout << "\n--- ChatCompletion Response ---\n";
      std::cout << "Endpoint:       " << host << ":" << port << "\n";
      std::cout << "ID:             " << resp.id << "\n";
      std::cout << "Model:          " << resp.model << "\n";
      std::cout << "Correlation ID: " << resp.correlation_id << "\n";
      std::cout << "Cache Hit:      " << (resp.cache_hit ? "true" : "false") << "\n";
      if (!resp.cache_tier.empty()) {
        std::cout << "Cache Tier:     " << resp.cache_tier << "\n";
      }
      if (!resp.choices.empty()) {
        std::cout << "Role:           " << resp.choices[0].message.role << "\n";
        std::cout << "Content:        " << resp.choices[0].message.content << "\n";
        std::cout << "Finish Reason:  " << resp.choices[0].finish_reason << "\n";
      }
      std::cout << "Prompt Tokens:  " << resp.usage.prompt_tokens << "\n";
      std::cout << "Compl Tokens:   " << resp.usage.completion_tokens << "\n";
      std::cout << "Total Tokens:   " << resp.usage.total_tokens << "\n";
      std::cout << "---------------------------------\n";
    }
  }

  if (fetch_metrics) {
    tessera::HttpClientResponse mresp;
    tessera::Status st = tessera::HttpRequestSync(
        host, metrics_port, "GET", "/metrics", "", {}, &mresp, timeout_ms);
    if (!st.ok() || mresp.status_code != 200) {
      std::cerr << "Failed fetching metrics from http://" << host << ":" << metrics_port
                << "/metrics: " << (st.ok() ? std::to_string(mresp.status_code) : st.message) << "\n";
      return 1;
    }
    std::cout << "\n# --- Prometheus Metrics Exposition ---\n";
    std::cout << mresp.body;
    std::cout << "# -------------------------------------\n";
  }

  return 0;
}
