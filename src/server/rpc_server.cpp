#include "tessera/server/rpc_server.hpp"
#include "tessera/common/json.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <queue>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef TESSERA_HAS_SYSTEM_GRPC
#include "gateway.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#endif

namespace tessera {

#ifdef TESSERA_HAS_SYSTEM_GRPC

namespace {

grpc::StatusCode ToGrpcStatusCode(StatusCode code) {
  switch (code) {
    case StatusCode::OK: return grpc::StatusCode::OK;
    case StatusCode::CANCELLED: return grpc::StatusCode::CANCELLED;
    case StatusCode::UNKNOWN: return grpc::StatusCode::UNKNOWN;
    case StatusCode::INVALID_ARGUMENT: return grpc::StatusCode::INVALID_ARGUMENT;
    case StatusCode::DEADLINE_EXCEEDED: return grpc::StatusCode::DEADLINE_EXCEEDED;
    case StatusCode::NOT_FOUND: return grpc::StatusCode::NOT_FOUND;
    case StatusCode::ALREADY_EXISTS: return grpc::StatusCode::ALREADY_EXISTS;
    case StatusCode::PERMISSION_DENIED: return grpc::StatusCode::PERMISSION_DENIED;
    case StatusCode::RESOURCE_EXHAUSTED: return grpc::StatusCode::RESOURCE_EXHAUSTED;
    case StatusCode::FAILED_PRECONDITION: return grpc::StatusCode::FAILED_PRECONDITION;
    case StatusCode::ABORTED: return grpc::StatusCode::ABORTED;
    case StatusCode::OUT_OF_RANGE: return grpc::StatusCode::OUT_OF_RANGE;
    case StatusCode::UNIMPLEMENTED: return grpc::StatusCode::UNIMPLEMENTED;
    case StatusCode::INTERNAL: return grpc::StatusCode::INTERNAL;
    case StatusCode::UNAVAILABLE: return grpc::StatusCode::UNAVAILABLE;
    case StatusCode::DATA_LOSS: return grpc::StatusCode::DATA_LOSS;
    case StatusCode::UNAUTHENTICATED: return grpc::StatusCode::UNAUTHENTICATED;
  }
  return grpc::StatusCode::UNKNOWN;
}

class GrpcCallTag {
 public:
  virtual ~GrpcCallTag() = default;
  virtual void Proceed(bool ok) = 0;
};

class GrpcHealthCall : public GrpcCallTag {
 public:
  GrpcHealthCall(
      tessera::gateway::v1::GatewayService::AsyncService* service,
      grpc::ServerCompletionQueue* cq,
      std::atomic<bool>* stop_flag,
      RpcServer::HealthQueryFn* on_health)
      : service_(service),
        cq_(cq),
        stop_flag_(stop_flag),
        on_health_(on_health),
        responder_(&ctx_) {
    Proceed(true);
  }

  void Proceed(bool ok) override {
    if (state_ == State::CREATE) {
      state_ = State::PROCESS;
      service_->RequestCheck(&ctx_, &req_, &responder_, cq_, cq_, this);
      return;
    }
    if (state_ == State::PROCESS) {
      if (!stop_flag_->load(std::memory_order_acquire)) {
        new GrpcHealthCall(service_, cq_, stop_flag_, on_health_);
      }
      if (!ok) {
        delete this;
        return;
      }
      HealthStatus hs = on_health_ ? (*on_health_)() : HealthStatus::UNKNOWN;
      switch (hs) {
        case HealthStatus::SERVING:
          resp_.set_status(tessera::gateway::v1::HealthCheckResponse::SERVING);
          break;
        case HealthStatus::NOT_SERVING:
          resp_.set_status(tessera::gateway::v1::HealthCheckResponse::NOT_SERVING);
          break;
        case HealthStatus::UNKNOWN:
          resp_.set_status(tessera::gateway::v1::HealthCheckResponse::UNKNOWN);
          break;
      }
      state_ = State::FINISH;
      responder_.Finish(resp_, grpc::Status::OK, this);
      return;
    }
    delete this;
  }

 private:
  enum class State { CREATE, PROCESS, FINISH };
  State state_{State::CREATE};
  tessera::gateway::v1::GatewayService::AsyncService* service_;
  grpc::ServerCompletionQueue* cq_;
  std::atomic<bool>* stop_flag_;
  RpcServer::HealthQueryFn* on_health_;
  grpc::ServerContext ctx_;
  tessera::gateway::v1::HealthCheckRequest req_;
  tessera::gateway::v1::HealthCheckResponse resp_;
  grpc::ServerAsyncResponseWriter<tessera::gateway::v1::HealthCheckResponse> responder_;
};

class GrpcCompletionCall : public GrpcCallTag {
 public:
  GrpcCompletionCall(
      tessera::gateway::v1::GatewayService::AsyncService* service,
      grpc::ServerCompletionQueue* cq,
      std::atomic<bool>* stop_flag,
      RpcServer::CompletionDispatchFn* on_completion)
      : service_(service),
        cq_(cq),
        stop_flag_(stop_flag),
        on_completion_(on_completion),
        responder_(&ctx_) {
    Proceed(true);
  }

  void Proceed(bool ok) override {
    if (state_ == State::CREATE) {
      state_ = State::PROCESS;
      service_->RequestChatCompletion(&ctx_, &pb_req_, &responder_, cq_, cq_, this);
      return;
    }
    if (state_ == State::PROCESS) {
      if (!stop_flag_->load(std::memory_order_acquire)) {
        new GrpcCompletionCall(service_, cq_, stop_flag_, on_completion_);
      }
      if (!ok) {
        delete this;
        return;
      }

      ChatCompletionRequest req;
      req.model = pb_req_.model();
      for (const auto& m : pb_req_.messages()) {
        req.messages.push_back(ChatMessage{.role = m.role(), .content = m.content(), .name = m.name()});
      }
      if (pb_req_.has_temperature()) {
        req.temperature = pb_req_.temperature();
      } else {
        req.temperature.reset();
      }
      if (pb_req_.has_max_tokens()) {
        req.max_tokens = pb_req_.max_tokens();
      } else {
        req.max_tokens.reset();
      }
      req.stream = pb_req_.stream();
      req.correlation_id = pb_req_.correlation_id();
      req.no_cache = pb_req_.no_cache();

      state_ = State::FINISH;
      if (on_completion_ && *on_completion_) {
        (*on_completion_)(
            std::move(req),
            [this](const ChatCompletionResponse& resp, Status status) {
              pb_resp_.set_id(resp.id);
              pb_resp_.set_created(resp.created);
              pb_resp_.set_model(resp.model);
              pb_resp_.set_correlation_id(resp.correlation_id);
              pb_resp_.set_cache_hit(resp.cache_hit);
              pb_resp_.set_cache_tier(resp.cache_tier);
              for (const auto& c : resp.choices) {
                auto* out_c = pb_resp_.add_choices();
                out_c->set_index(c.index);
                out_c->set_finish_reason(c.finish_reason);
                auto* out_m = out_c->mutable_message();
                out_m->set_role(c.message.role);
                out_m->set_content(c.message.content);
                out_m->set_name(c.message.name);
              }
              auto* u = pb_resp_.mutable_usage();
              u->set_prompt_tokens(resp.usage.prompt_tokens);
              u->set_completion_tokens(resp.usage.completion_tokens);
              u->set_total_tokens(resp.usage.total_tokens);

              grpc::Status grpc_status = status.ok()
                  ? grpc::Status::OK
                  : grpc::Status(ToGrpcStatusCode(status.code), status.message);
              responder_.Finish(pb_resp_, grpc_status, this);
            });
      } else {
        responder_.Finish(pb_resp_, grpc::Status(grpc::StatusCode::INTERNAL, "No backend handler"), this);
      }
      return;
    }
    delete this;
  }

 private:
  enum class State { CREATE, PROCESS, FINISH };
  State state_{State::CREATE};
  tessera::gateway::v1::GatewayService::AsyncService* service_;
  grpc::ServerCompletionQueue* cq_;
  std::atomic<bool>* stop_flag_;
  RpcServer::CompletionDispatchFn* on_completion_;
  grpc::ServerContext ctx_;
  tessera::gateway::v1::ChatCompletionRequest pb_req_;
  tessera::gateway::v1::ChatCompletionResponse pb_resp_;
  grpc::ServerAsyncResponseWriter<tessera::gateway::v1::ChatCompletionResponse> responder_;
};

}  // namespace

struct RpcServer::Impl {
  std::string host;
  uint16_t requested_port;
  uint32_t worker_threads;
  size_t max_message_size;
  CompletionDispatchFn on_completion;
  HealthQueryFn on_health;

  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<uint16_t> bound_port{0};

  tessera::gateway::v1::GatewayService::AsyncService service;
  std::unique_ptr<grpc::ServerCompletionQueue> cq;
  std::unique_ptr<grpc::Server> server;
  std::vector<std::thread> workers;

  bool Start() {
    if (running.load(std::memory_order_acquire)) return true;
    stop_requested.store(false, std::memory_order_release);

    std::string bind_host = host.empty() ? "0.0.0.0" : host;
    std::string addr = bind_host + ":" + std::to_string(requested_port);

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &selected_port);
    int max_bytes = static_cast<int>(
        std::min(max_message_size, static_cast<size_t>(INT32_MAX)));
    builder.SetMaxReceiveMessageSize(max_bytes);
    builder.SetMaxSendMessageSize(max_bytes);
    builder.RegisterService(&service);
    cq = builder.AddCompletionQueue();
    server = builder.BuildAndStart();

    if (!server || !cq || selected_port <= 0) {
      TESSERA_LOG_ERROR("Async gRPC server failed to bind {}", addr);
      if (server) server->Shutdown();
      if (cq) cq->Shutdown();
      server.reset();
      cq.reset();
      bound_port.store(0, std::memory_order_release);
      return false;
    }

    bound_port.store(static_cast<uint16_t>(selected_port), std::memory_order_release);
    running.store(true, std::memory_order_release);

    // Seed initial acceptors for ChatCompletion and Check
    new GrpcCompletionCall(&service, cq.get(), &stop_requested, &on_completion);
    new GrpcHealthCall(&service, cq.get(), &stop_requested, &on_health);

    uint32_t threads = std::max(1u, worker_threads);
    workers.reserve(threads);
    for (uint32_t i = 0; i < threads; ++i) {
      workers.emplace_back([this]() {
        void* tag = nullptr;
        bool ok = false;
        while (cq->Next(&tag, &ok)) {
          auto* call = static_cast<GrpcCallTag*>(tag);
          call->Proceed(ok);
        }
      });
    }
    return true;
  }

  void Stop() {
    if (!running.exchange(false, std::memory_order_acq_rel)) return;
    stop_requested.store(true, std::memory_order_release);

    if (server) {
      auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(500);
      server->Shutdown(deadline);
    }
    if (cq) {
      cq->Shutdown();
    }
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
    workers.clear();
    server.reset();
    cq.reset();
    bound_port.store(0, std::memory_order_release);
  }
};

#else

namespace {

constexpr size_t kMaxRpcPendingQueue = 512;
constexpr size_t kFrameHeaderSize = 4;

bool SendAllTcp(int fd, const void* data, size_t len) {
  const auto* ptr = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool RecvExactTcp(int fd, void* data, size_t len) {
  auto* ptr = static_cast<char*>(data);
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::recv(fd, ptr + got, len - got, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

bool SendTcpFrame(int fd, std::string_view payload) {
  uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
  if (!SendAllTcp(fd, &net_len, kFrameHeaderSize)) return false;
  if (!payload.empty() && !SendAllTcp(fd, payload.data(), payload.size())) return false;
  return true;
}

}  // namespace

struct RpcServer::Impl {
  std::string host;
  uint16_t requested_port;
  uint32_t worker_threads;
  size_t max_message_size;
  CompletionDispatchFn on_completion;
  HealthQueryFn on_health;

  int listen_fd{-1};
  int wakeup_fds[2]{-1, -1};
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<uint16_t> bound_port{0};

  std::thread accept_thread;
  std::vector<std::thread> workers;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::queue<int> pending_fds;

  void CleanupStartupFds() noexcept {
    if (listen_fd >= 0) {
      ::close(listen_fd);
      listen_fd = -1;
    }
    if (wakeup_fds[0] >= 0) {
      ::close(wakeup_fds[0]);
      wakeup_fds[0] = -1;
    }
    if (wakeup_fds[1] >= 0) {
      ::close(wakeup_fds[1]);
      wakeup_fds[1] = -1;
    }
    bound_port.store(0, std::memory_order_release);
  }

  bool Start() {
    if (running.load(std::memory_order_acquire)) return true;

    if (::pipe(wakeup_fds) != 0) {
      TESSERA_LOG_ERROR("RpcServer failed to create wakeup pipe: {}", std::strerror(errno));
      CleanupStartupFds();
      return false;
    }
    for (int i = 0; i < 2; ++i) {
      int flags = ::fcntl(wakeup_fds[i], F_GETFL, 0);
      if (flags >= 0) ::fcntl(wakeup_fds[i], F_SETFL, flags | O_NONBLOCK);
      ::fcntl(wakeup_fds[i], F_SETFD, FD_CLOEXEC);
    }

    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      TESSERA_LOG_ERROR("RpcServer socket() failed: {}", std::strerror(errno));
      CleanupStartupFds();
      return false;
    }
    ::fcntl(listen_fd, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int lflags = ::fcntl(listen_fd, F_GETFL, 0);
    if (lflags >= 0) ::fcntl(listen_fd, F_SETFL, lflags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(requested_port);
    if (host.empty() || host == "0.0.0.0") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      TESSERA_LOG_ERROR("RpcServer invalid bind IP address '{}'", host);
      CleanupStartupFds();
      return false;
    }

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      TESSERA_LOG_ERROR("RpcServer bind() failed on {}:{}: {}", host, requested_port, std::strerror(errno));
      CleanupStartupFds();
      return false;
    }

    if (::listen(listen_fd, 128) != 0) {
      TESSERA_LOG_ERROR("RpcServer listen() failed on {}:{}: {}", host, requested_port, std::strerror(errno));
      CleanupStartupFds();
      return false;
    }

    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
      bound_port.store(ntohs(actual.sin_port), std::memory_order_release);
    } else {
      bound_port.store(requested_port, std::memory_order_release);
    }

    stop_requested.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);

    uint32_t threads = std::max(1u, worker_threads);
    workers.reserve(threads);
    for (uint32_t i = 0; i < threads; ++i) {
      workers.emplace_back([this]() { WorkerLoop(); });
    }
    accept_thread = std::thread([this]() { AcceptLoop(); });
    return true;
  }

  void Stop() {
    if (!running.exchange(false, std::memory_order_acq_rel)) return;
    stop_requested.store(true, std::memory_order_release);

    if (wakeup_fds[1] >= 0) {
      char b = 1;
      ssize_t ignored = ::write(wakeup_fds[1], &b, 1);
      (void)ignored;
    }
    if (listen_fd >= 0) {
      ::shutdown(listen_fd, SHUT_RDWR);
    }

    if (accept_thread.joinable()) accept_thread.join();
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      queue_cv.notify_all();
    }
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
    workers.clear();

    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      while (!pending_fds.empty()) {
        ::close(pending_fds.front());
        pending_fds.pop();
      }
    }
    CleanupStartupFds();
  }

  void AcceptLoop() {
    while (!stop_requested.load(std::memory_order_acquire)) {
      pollfd pfds[2]{};
      pfds[0].fd = listen_fd;
      pfds[0].events = POLLIN;
      pfds[1].fd = wakeup_fds[0];
      pfds[1].events = POLLIN;

      int rc = ::poll(pfds, 2, -1);
      if (rc < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if ((pfds[1].revents & POLLIN) != 0 || stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      if ((pfds[0].revents & POLLIN) != 0) {
        while (!stop_requested.load(std::memory_order_acquire)) {
          sockaddr_in caddr{};
          socklen_t clen = sizeof(caddr);
          int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
          if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            break;
          }
          ::fcntl(cfd, F_SETFD, FD_CLOEXEC);

          bool enqueued = false;
          {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!stop_requested.load(std::memory_order_acquire) &&
                pending_fds.size() < kMaxRpcPendingQueue) {
              pending_fds.push(cfd);
              enqueued = true;
              queue_cv.notify_one();
            }
          }
          if (!enqueued) {
            JsonValue err = JsonValue::MakeObject();
            err.Set("status_code", static_cast<int>(StatusCode::UNAVAILABLE));
            err.Set("status_message", "RPC server overloaded or shutting down");
            SendTcpFrame(cfd, err.Dump());
            ::close(cfd);
          }
        }
      }
    }
  }

  void WorkerLoop() {
    while (true) {
      int cfd = -1;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [this]() {
          return stop_requested.load(std::memory_order_acquire) || !pending_fds.empty();
        });
        if (stop_requested.load(std::memory_order_acquire) && pending_fds.empty()) {
          break;
        }
        cfd = pending_fds.front();
        pending_fds.pop();
      }
      if (cfd >= 0) {
        HandleClient(cfd);
        ::close(cfd);
      }
    }
  }

  void HandleClient(int cfd) {
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint32_t net_len = 0;
    if (!RecvExactTcp(cfd, &net_len, kFrameHeaderSize)) {
      return;
    }
    uint32_t frame_len = ntohl(net_len);
    size_t max_frame = max_message_size + 65536u;
    if (frame_len == 0 || static_cast<size_t>(frame_len) > max_frame) {
      JsonValue err = JsonValue::MakeObject();
      err.Set("status_code", static_cast<int>(StatusCode::RESOURCE_EXHAUSTED));
      err.Set("status_message",
              "RPC frame size " + std::to_string(frame_len) + " exceeds limit " +
                  std::to_string(max_message_size));
      SendTcpFrame(cfd, err.Dump());
      return;
    }

    std::string payload(frame_len, '\0');
    if (!RecvExactTcp(cfd, payload.data(), frame_len)) {
      return;
    }

    JsonValue envelope;
    std::string parse_err;
    if (!JsonValue::Parse(payload, &envelope, &parse_err) || !envelope.IsObject()) {
      JsonValue err = JsonValue::MakeObject();
      err.Set("status_code", static_cast<int>(StatusCode::INVALID_ARGUMENT));
      err.Set("status_message", "Invalid RPC JSON envelope: " + parse_err);
      SendTcpFrame(cfd, err.Dump());
      return;
    }

    const JsonValue* method_val = envelope.Find("method");
    if (method_val == nullptr || !method_val->IsString()) {
      JsonValue err = JsonValue::MakeObject();
      err.Set("status_code", static_cast<int>(StatusCode::INVALID_ARGUMENT));
      err.Set("status_message", "Missing RPC 'method' field");
      SendTcpFrame(cfd, err.Dump());
      return;
    }

    const std::string& method = method_val->AsString();
    if (method == "Check") {
      HealthStatus hs = on_health ? on_health() : HealthStatus::UNKNOWN;
      JsonValue out = JsonValue::MakeObject();
      out.Set("status", std::string(HealthStatusToString(hs)));
      SendTcpFrame(cfd, out.Dump());
      return;
    }

    if (method == "ChatCompletion") {
      const JsonValue* req_val = envelope.Find("request");
      ChatCompletionRequest req;
      std::string err_msg;
      if (req_val == nullptr || !json_codec::JsonToChatRequest(*req_val, &req, &err_msg)) {
        JsonValue err = JsonValue::MakeObject();
        err.Set("status_code", static_cast<int>(StatusCode::INVALID_ARGUMENT));
        err.Set("status_message", err_msg.empty() ? "Missing 'request' object" : err_msg);
        SendTcpFrame(cfd, err.Dump());
        return;
      }

      if (!on_completion) {
        JsonValue err = JsonValue::MakeObject();
        err.Set("status_code", static_cast<int>(StatusCode::INTERNAL));
        err.Set("status_message", "No completion dispatcher attached");
        SendTcpFrame(cfd, err.Dump());
        return;
      }

      ChatCompletionResponse resp;
      Status status = Status::OK();
      std::mutex mtx;
      std::condition_variable cv;
      bool done = false;

      on_completion(
          std::move(req),
          [&](const ChatCompletionResponse& r, Status st) {
            std::lock_guard<std::mutex> lock(mtx);
            resp = r;
            status = std::move(st);
            done = true;
            cv.notify_one();
          });

      {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]() { return done; });
      }

      JsonValue out = JsonValue::MakeObject();
      out.Set("status_code", static_cast<int>(status.code));
      out.Set("status_message", status.message);
      out.Set("response", json_codec::ChatResponseToJson(resp));
      SendTcpFrame(cfd, out.Dump());
      return;
    }

    JsonValue err = JsonValue::MakeObject();
    err.Set("status_code", static_cast<int>(StatusCode::UNIMPLEMENTED));
    err.Set("status_message", "Unsupported RPC method '" + method + "'");
    SendTcpFrame(cfd, err.Dump());
  }
};

#endif

RpcServer::RpcServer(
    std::string host,
    uint16_t port,
    uint32_t worker_threads,
    size_t max_message_size,
    CompletionDispatchFn on_completion,
    HealthQueryFn on_health)
    : impl_(std::make_unique<Impl>()) {
  impl_->host = std::move(host);
  impl_->requested_port = port;
  impl_->worker_threads = worker_threads;
  impl_->max_message_size = max_message_size;
  impl_->on_completion = std::move(on_completion);
  impl_->on_health = std::move(on_health);
}

RpcServer::~RpcServer() {
  Stop();
}

bool RpcServer::Start() {
  return impl_->Start();
}

void RpcServer::Stop() {
  if (impl_) impl_->Stop();
}

bool RpcServer::IsRunning() const noexcept {
  return impl_ && impl_->running.load(std::memory_order_acquire);
}

uint16_t RpcServer::BoundPort() const noexcept {
  return impl_ ? impl_->bound_port.load(std::memory_order_acquire) : 0;
}

}  // namespace tessera
