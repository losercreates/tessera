#pragma once

#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <mutex>
#include <chrono>

namespace tessera {

enum class LogLevel {
  DEBUG = 0,
  INFO = 1,
  WARN = 2,
  ERROR = 3
};

class Logger {
 public:
  static Logger& Instance() {
    static Logger logger;
    return logger;
  }

  void SetLevel(LogLevel level) noexcept { min_level_ = level; }
  [[nodiscard]] LogLevel GetLevel() const noexcept { return min_level_; }

  template <typename... Args>
  void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < min_level_) return;
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    WriteLog(level, msg);
  }

  template <typename... Args>
  void Debug(std::format_string<Args...> fmt, Args&&... args) {
    Log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void Info(std::format_string<Args...> fmt, Args&&... args) {
    Log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void Warn(std::format_string<Args...> fmt, Args&&... args) {
    Log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void Error(std::format_string<Args...> fmt, Args&&... args) {
    Log(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
  }

 private:
  Logger() = default;
  void WriteLog(LogLevel level, const std::string& msg);

  LogLevel min_level_{LogLevel::INFO};
  std::mutex mutex_;
};

#define TESSERA_LOG_DEBUG(...) ::tessera::Logger::Instance().Debug(__VA_ARGS__)
#define TESSERA_LOG_INFO(...)  ::tessera::Logger::Instance().Info(__VA_ARGS__)
#define TESSERA_LOG_WARN(...)  ::tessera::Logger::Instance().Warn(__VA_ARGS__)
#define TESSERA_LOG_ERROR(...) ::tessera::Logger::Instance().Error(__VA_ARGS__)

}  // namespace tessera

