#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  bool check_health = false;
  std::string user_msg = "Hello, gateway!";
  std::string model = "llama-3.3-70b";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--health") {
      check_health = true;
    } else if (arg.rfind("--message=", 0) == 0) {
      user_msg = arg.substr(10);
    } else if (arg == "--message" && i + 1 < argc) {
      user_msg = argv[++i];
    } else if (arg.rfind("--model=", 0) == 0) {
      model = arg.substr(8);
    }
  }

  tessera::ServerConfig config;
  auto backend = std::make_shared<tessera::MockEchoBackend>();
  tessera::GatewayServer server(config, backend);
  server.Start();

  if (check_health) {
    tessera::HealthStatus status = server.CheckHealth();
    std::cout << "Health Check: " << tessera::HealthStatusToString(status) << "\n";
    server.Stop();
    return status == tessera::HealthStatus::SERVING ? 0 : 1;
  }

  tessera::ChatCompletionRequest req;
  req.model = model;
  req.messages.push_back(tessera::ChatMessage{.role = "system", .content = "You are a helpful assistant."});
  req.messages.push_back(tessera::ChatMessage{.role = "user", .content = user_msg});

  tessera::ChatCompletionResponse resp;
  tessera::Status s = server.ProcessRequestSync(req, &resp);

  server.Stop();

  if (!s.ok()) {
    std::cerr << "Request failed: [" << tessera::StatusCodeToString(s.code) << "] " << s.message << "\n";
    return 1;
  }

  std::cout << "\n--- ChatCompletion Response ---\n";
  std::cout << "ID:             " << resp.id << "\n";
  std::cout << "Model:          " << resp.model << "\n";
  std::cout << "Correlation ID: " << resp.correlation_id << "\n";
  if (!resp.choices.empty()) {
    std::cout << "Role:           " << resp.choices[0].message.role << "\n";
    std::cout << "Content:        " << resp.choices[0].message.content << "\n";
    std::cout << "Finish Reason:  " << resp.choices[0].finish_reason << "\n";
  }
  std::cout << "Prompt Tokens:  " << resp.usage.prompt_tokens << "\n";
  std::cout << "Compl Tokens:   " << resp.usage.completion_tokens << "\n";
  std::cout << "Total Tokens:   " << resp.usage.total_tokens << "\n";
  std::cout << "---------------------------------\n";

  return 0;
}

