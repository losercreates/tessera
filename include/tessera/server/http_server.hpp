#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::unordered_map<std::string, std::string> headers;  // lowercased header names
  std::string body;

  [[nodiscard]] std::string Header(std::string_view key) const;
};

class HttpResponseWriter {
 public:
  virtual ~HttpResponseWriter() = default;

  virtual bool SendResponse(
      int status_code,
      std::string_view status_text,
      std::string_view content_type,
      std::string_view body,
      const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) = 0;

  virtual bool BeginSseStream(
      const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) = 0;

  virtual bool WriteSseData(std::string_view data_payload) = 0;
};

using HttpRequestHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

/**
 * Reusable, zero-dependency POSIX HTTP/1.1 + SSE server.
 * Supports SO_REUSEADDR, ephemeral port 0, self-pipe poll wakeup for instant shutdown,
 * bounded body reads, and SIGPIPE-safe (MSG_NOSIGNAL) incremental chunk flushing.
 */
class HttpServer {
 public:
  HttpServer(
      std::string host,
      uint16_t port,
      uint32_t worker_threads,
      size_t max_body_bytes,
      HttpRequestHandler handler);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool Start();
  void Stop();

  [[nodiscard]] bool IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint16_t BoundPort() const noexcept {
    return bound_port_.load(std::memory_order_acquire);
  }

 private:
  void AcceptLoop();
  void WorkerLoop();
  void HandleConnection(int client_fd);
  void CleanupStartupFds() noexcept;

  std::string host_;
  uint16_t requested_port_;
  uint32_t worker_threads_;
  size_t max_body_bytes_;
  HttpRequestHandler handler_;

  int listen_fd_{-1};
  int wakeup_fds_[2]{-1, -1};
  std::atomic<uint16_t> bound_port_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  std::thread accept_thread_;
  std::vector<std::thread> workers_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::queue<int> pending_fds_;
};

}  // namespace tessera
