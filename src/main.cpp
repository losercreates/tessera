#include "tessera/server/gateway_server.hpp"
#include "tessera/backend/mock_backend.hpp"
#include "tessera/common/logger.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  tessera::ServerConfig config;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--port=", 0) == 0) {
      config.port = static_cast<uint16_t>(std::stoi(arg.substr(7)));
    } else if (arg.rfind("--threads=", 0) == 0) {
      config.worker_threads = static_cast<uint32_t>(std::stoi(arg.substr(10)));
    } else if (arg.rfind("--max_mb=", 0) == 0) {
      config.max_message_size = static_cast<size_t>(std::stoi(arg.substr(9))) * 1024 * 1024;
    } else if (arg.rfind("--log_level=", 0) == 0) {
      std::string lvl = arg.substr(12);
      if (lvl == "debug") tessera::Logger::Instance().SetLevel(tessera::LogLevel::DEBUG);
      else if (lvl == "warn") tessera::Logger::Instance().SetLevel(tessera::LogLevel::WARN);
      else if (lvl == "error") tessera::Logger::Instance().SetLevel(tessera::LogLevel::ERROR);
      else tessera::Logger::Instance().SetLevel(tessera::LogLevel::INFO);
    }
  }

  auto backend = std::make_shared<tessera::MockEchoBackend>();
  tessera::GatewayServer server(config, backend);

  tessera::GatewayServer::RegisterSignalHandler(&server);

  if (!server.Start()) {
    TESSERA_LOG_ERROR("Failed to start gateway server on port {}", config.port);
    return 1;
  }

  std::cout << "\n=======================================================\n";
  std::cout << " Tessera Distributed LLM API Gateway (Milestone 1)     \n";
  std::cout << " Status: READY & SERVING                               \n";
  std::cout << " Port:   " << config.port << "                         \n";
  std::cout << " Threads:" << config.worker_threads << "               \n";
  std::cout << " Backend:" << backend->Name() << "                     \n";
  std::cout << "=======================================================\n\n";

  server.Wait();
  return 0;
}

