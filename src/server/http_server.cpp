#include "tessera/server/http_server.hpp"
#include "tessera/common/json.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace tessera {

namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxPendingQueue = 512;

std::string ToLowerAscii(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char ch : sv) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string_view TrimAscii(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
    sv.remove_suffix(1);
  }
  return sv;
}

bool SendAllFd(int fd, std::string_view data) {
  size_t sent_total = 0;
  while (sent_total < data.size()) {
    ssize_t n = ::send(
        fd,
        data.data() + sent_total,
        data.size() - sent_total,
        MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent_total += static_cast<size_t>(n);
  }
  return true;
}

class SocketHttpResponseWriter : public HttpResponseWriter {
 public:
  explicit SocketHttpResponseWriter(int fd) : fd_(fd) {}

  bool SendResponse(
      int status_code,
      std::string_view status_text,
      std::string_view content_type,
      std::string_view body,
      const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) override {
    if (headers_sent_) return false;
    headers_sent_ = true;

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n";
    for (const auto& [k, v] : extra_headers) {
      oss << k << ": " << v << "\r\n";
    }
    oss << "\r\n";

    std::string head = oss.str();
    if (!SendAllFd(fd_, head)) return false;
    if (!body.empty() && !SendAllFd(fd_, body)) return false;
    return true;
  }

  bool BeginSseStream(
      const std::vector<std::pair<std::string, std::string>>& extra_headers) override {
    if (headers_sent_) return false;
    headers_sent_ = true;

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Content-Type: text/event-stream\r\n";
    oss << "Cache-Control: no-cache\r\n";
    oss << "Connection: close\r\n";
    for (const auto& [k, v] : extra_headers) {
      oss << k << ": " << v << "\r\n";
    }
    oss << "\r\n";
    return SendAllFd(fd_, oss.str());
  }

  bool WriteSseData(std::string_view data_payload) override {
    std::string frame;
    frame.reserve(data_payload.size() + 8);
    frame.append("data: ");
    frame.append(data_payload);
    frame.append("\n\n");
    return SendAllFd(fd_, frame);
  }

  [[nodiscard]] bool HeadersSent() const noexcept { return headers_sent_; }

 private:
  int fd_;
  bool headers_sent_{false};
};

}  // namespace

std::string HttpRequest::Header(std::string_view key) const {
  std::string lower = ToLowerAscii(key);
  auto it = headers.find(lower);
  if (it != headers.end()) {
    return it->second;
  }
  return "";
}

HttpServer::HttpServer(
    std::string host,
    uint16_t port,
    uint32_t worker_threads,
    size_t max_body_bytes,
    HttpRequestHandler handler)
    : host_(std::move(host)),
      requested_port_(port),
      worker_threads_(std::max(1u, worker_threads)),
      max_body_bytes_(max_body_bytes),
      handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
  Stop();
}

void HttpServer::CleanupStartupFds() noexcept {
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (wakeup_fds_[0] >= 0) {
    ::close(wakeup_fds_[0]);
    wakeup_fds_[0] = -1;
  }
  if (wakeup_fds_[1] >= 0) {
    ::close(wakeup_fds_[1]);
    wakeup_fds_[1] = -1;
  }
  bound_port_.store(0, std::memory_order_release);
}

bool HttpServer::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return true;
  }

  if (::pipe(wakeup_fds_) != 0) {
    TESSERA_LOG_ERROR("HttpServer failed to create wakeup pipe: {}", std::strerror(errno));
    CleanupStartupFds();
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    int flags = ::fcntl(wakeup_fds_[i], F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(wakeup_fds_[i], F_SETFL, flags | O_NONBLOCK);
    }
    ::fcntl(wakeup_fds_[i], F_SETFD, FD_CLOEXEC);
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    TESSERA_LOG_ERROR("HttpServer failed to create socket: {}", std::strerror(errno));
    CleanupStartupFds();
    return false;
  }
  ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  int lflags = ::fcntl(listen_fd_, F_GETFL, 0);
  if (lflags >= 0) {
    ::fcntl(listen_fd_, F_SETFL, lflags | O_NONBLOCK);
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(requested_port_);
  if (host_.empty() || host_ == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    TESSERA_LOG_ERROR("HttpServer invalid bind IP address '{}'", host_);
    CleanupStartupFds();
    return false;
  }

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    TESSERA_LOG_ERROR("HttpServer failed to bind {}:{}: {}", host_, requested_port_, std::strerror(errno));
    CleanupStartupFds();
    return false;
  }

  if (::listen(listen_fd_, 128) != 0) {
    TESSERA_LOG_ERROR("HttpServer listen() failed on {}:{}: {}", host_, requested_port_, std::strerror(errno));
    CleanupStartupFds();
    return false;
  }

  sockaddr_in actual{};
  socklen_t actual_len = sizeof(actual);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
    bound_port_.store(ntohs(actual.sin_port), std::memory_order_release);
  } else {
    bound_port_.store(requested_port_, std::memory_order_release);
  }

  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);

  workers_.reserve(worker_threads_);
  for (uint32_t i = 0; i < worker_threads_; ++i) {
    workers_.emplace_back(&HttpServer::WorkerLoop, this);
  }
  accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);

  return true;
}

void HttpServer::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  stop_requested_.store(true, std::memory_order_release);
  if (wakeup_fds_[1] >= 0) {
    char b = 1;
    ssize_t ignored = ::write(wakeup_fds_[1], &b, 1);
    (void)ignored;
  }
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_all();
  }
  for (auto& w : workers_) {
    if (w.joinable()) {
      w.join();
    }
  }
  workers_.clear();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!pending_fds_.empty()) {
      ::close(pending_fds_.front());
      pending_fds_.pop();
    }
  }

  CleanupStartupFds();
}

void HttpServer::AcceptLoop() {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    pollfd pfds[2]{};
    pfds[0].fd = listen_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = wakeup_fds_[0];
    pfds[1].events = POLLIN;

    int rc = ::poll(pfds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if ((pfds[1].revents & POLLIN) != 0 || stop_requested_.load(std::memory_order_acquire)) {
      break;
    }

    if ((pfds[0].revents & POLLIN) != 0) {
      while (!stop_requested_.load(std::memory_order_acquire)) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
          }
          break;
        }
        ::fcntl(client_fd, F_SETFD, FD_CLOEXEC);

        bool enqueued = false;
        {
          std::lock_guard<std::mutex> lock(queue_mutex_);
          if (!stop_requested_.load(std::memory_order_acquire) &&
              pending_fds_.size() < kMaxPendingQueue) {
            pending_fds_.push(client_fd);
            enqueued = true;
            queue_cv_.notify_one();
          }
        }
        if (!enqueued) {
          SocketHttpResponseWriter writer(client_fd);
          writer.SendResponse(
              503,
              "Service Unavailable",
              "application/json",
              json_codec::SerializeOpenAIError("Server queue saturated or shutting down", "server_error", "overloaded"));
          ::close(client_fd);
        }
      }
    }
  }
}

void HttpServer::WorkerLoop() {
  while (true) {
    int client_fd = -1;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() {
        return stop_requested_.load(std::memory_order_acquire) || !pending_fds_.empty();
      });
      if (stop_requested_.load(std::memory_order_acquire) && pending_fds_.empty()) {
        break;
      }
      client_fd = pending_fds_.front();
      pending_fds_.pop();
    }
    if (client_fd >= 0) {
      HandleConnection(client_fd);
      ::close(client_fd);
    }
  }
}

void HttpServer::HandleConnection(int client_fd) {
  timeval tv{};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  SocketHttpResponseWriter writer(client_fd);

  std::string buffer;
  buffer.reserve(4096);
  size_t header_end = std::string::npos;

  char tmp[4096];
  while (header_end == std::string::npos) {
    ssize_t n = ::recv(client_fd, tmp, sizeof(tmp), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (n == 0) {
      return;
    }
    buffer.append(tmp, static_cast<size_t>(n));
    header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos && buffer.size() > kMaxHeaderBytes) {
      writer.SendResponse(
          431,
          "Request Header Fields Too Large",
          "application/json",
          json_codec::SerializeOpenAIError("HTTP headers exceed maximum size", "invalid_request_error", "headers_too_large"));
      return;
    }
  }

  std::string_view header_block(buffer.data(), header_end);
  auto first_crlf = header_block.find("\r\n");
  std::string_view request_line = (first_crlf == std::string_view::npos)
                                      ? header_block
                                      : header_block.substr(0, first_crlf);

  auto sp1 = request_line.find(' ');
  auto sp2 = (sp1 == std::string_view::npos) ? std::string_view::npos : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    writer.SendResponse(
        400,
        "Bad Request",
        "application/json",
        json_codec::SerializeOpenAIError("Malformed HTTP request line", "invalid_request_error"));
    return;
  }

  HttpRequest req;
  req.method = std::string(request_line.substr(0, sp1));
  std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  auto qmark = target.find('?');
  if (qmark != std::string_view::npos) {
    req.path = std::string(target.substr(0, qmark));
    req.query = std::string(target.substr(qmark + 1));
  } else {
    req.path = std::string(target);
  }

  size_t pos = (first_crlf == std::string_view::npos) ? header_block.size() : first_crlf + 2;
  while (pos < header_block.size()) {
    auto next_crlf = header_block.find("\r\n", pos);
    std::string_view line = (next_crlf == std::string_view::npos)
                                ? header_block.substr(pos)
                                : header_block.substr(pos, next_crlf - pos);
    pos = (next_crlf == std::string_view::npos) ? header_block.size() : next_crlf + 2;
    auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      std::string key = ToLowerAscii(TrimAscii(line.substr(0, colon)));
      std::string val(TrimAscii(line.substr(colon + 1)));
      req.headers[std::move(key)] = std::move(val);
    }
  }

  size_t content_length = 0;
  std::string cl_hdr = req.Header("content-length");
  if (!cl_hdr.empty()) {
    for (char ch : cl_hdr) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        writer.SendResponse(
            400,
            "Bad Request",
            "application/json",
            json_codec::SerializeOpenAIError("Invalid Content-Length header", "invalid_request_error"));
        return;
      }
    }
    try {
      content_length = std::stoull(cl_hdr);
    } catch (...) {
      writer.SendResponse(
          413,
          "Payload Too Large",
          "application/json",
          json_codec::SerializeOpenAIError("Content-Length overflow", "invalid_request_error", "payload_too_large"));
      return;
    }
    if (content_length > max_body_bytes_) {
      writer.SendResponse(
          413,
          "Payload Too Large",
          "application/json",
          json_codec::SerializeOpenAIError(
              "Request body exceeds maximum allowed size of " + std::to_string(max_body_bytes_) + " bytes",
              "invalid_request_error",
              "payload_too_large"));
      return;
    }
  }

  size_t body_offset = header_end + 4;
  size_t already_have = buffer.size() - body_offset;
  if (already_have > content_length && content_length > 0) {
    already_have = content_length;
  }

  if (content_length > 0) {
    req.body.reserve(content_length);
    req.body.append(buffer.data() + body_offset, already_have);
    while (req.body.size() < content_length) {
      size_t remain = content_length - req.body.size();
      size_t to_read = std::min(sizeof(tmp), remain);
      ssize_t n = ::recv(client_fd, tmp, to_read, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return;
      }
      if (n == 0) {
        return;  // Premature EOF
      }
      req.body.append(tmp, static_cast<size_t>(n));
    }
  }

  if (handler_) {
    handler_(req, writer);
  }
  if (!writer.HeadersSent()) {
    writer.SendResponse(
        500,
        "Internal Server Error",
        "application/json",
        json_codec::SerializeOpenAIError("Request handler produced no response", "server_error"));
  }
}

}  // namespace tessera
