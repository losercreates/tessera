#include "tessera/common/logger.hpp"

namespace tessera {

void Logger::WriteLog(LogLevel level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  const char* level_str = "INFO";
  switch (level) {
    case LogLevel::DEBUG: level_str = "DEBUG"; break;
    case LogLevel::INFO:  level_str = "INFO";  break;
    case LogLevel::WARN:  level_str = "WARN";  break;
    case LogLevel::ERROR: level_str = "ERROR"; break;
  }

  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  auto timer = std::chrono::system_clock::to_time_t(now);
  std::tm bt{};
  localtime_r(&timer, &bt);

  char time_buf[32];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &bt);

  std::cout << std::format("[{} .{:03d}] [{}] {}\n", time_buf, ms.count(), level_str, msg);
  std::cout.flush();
}

}  // namespace tessera

