#include "tessera/server/rpc_client.hpp"
#include "tessera/common/json.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef TESSERA_HAS_SYSTEM_GRPC
#include "gateway.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#endif

namespace tessera {

namespace {

std::string ToLowerHeader(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char ch : sv) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string_view TrimAsciiView(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
    sv.remove_suffix(1);
  }
  return sv;
}

int ConnectTcpHost(std::string_view host, uint16_t port, uint32_t timeout_ms, std::string* err_msg) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  std::string host_str(host);
  std::string port_str = std::to_string(port);
  addrinfo* res = nullptr;
  int gai = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0 || res == nullptr) {
    if (err_msg) {
      *err_msg = "Failed to resolve host '" + host_str + "': " + ::gai_strerror(gai);
    }
    return -1;
  }

  int fd = -1;
  int last_errno = 0;
  for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      last_errno = errno;
      continue;
    }

    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000u);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000u) * 1000u);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    last_errno = errno;
    ::close(fd);
    fd = -1;
  }

  ::freeaddrinfo(res);
  if (fd < 0 && err_msg) {
    *err_msg = "Failed to connect to " + host_str + ":" + port_str + ": " + std::strerror(last_errno);
  }
  return fd;
}

bool SendAllSocket(int fd, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

#ifndef TESSERA_HAS_SYSTEM_GRPC
bool RecvExactSocket(int fd, void* data, size_t len) {
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
#endif

#ifdef TESSERA_HAS_SYSTEM_GRPC
StatusCode FromGrpcStatusCode(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK: return StatusCode::OK;
    case grpc::StatusCode::CANCELLED: return StatusCode::CANCELLED;
    case grpc::StatusCode::UNKNOWN: return StatusCode::UNKNOWN;
    case grpc::StatusCode::INVALID_ARGUMENT: return StatusCode::INVALID_ARGUMENT;
    case grpc::StatusCode::DEADLINE_EXCEEDED: return StatusCode::DEADLINE_EXCEEDED;
    case grpc::StatusCode::NOT_FOUND: return StatusCode::NOT_FOUND;
    case grpc::StatusCode::ALREADY_EXISTS: return StatusCode::ALREADY_EXISTS;
    case grpc::StatusCode::PERMISSION_DENIED: return StatusCode::PERMISSION_DENIED;
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return StatusCode::RESOURCE_EXHAUSTED;
    case grpc::StatusCode::FAILED_PRECONDITION: return StatusCode::FAILED_PRECONDITION;
    case grpc::StatusCode::ABORTED: return StatusCode::ABORTED;
    case grpc::StatusCode::OUT_OF_RANGE: return StatusCode::OUT_OF_RANGE;
    case grpc::StatusCode::UNIMPLEMENTED: return StatusCode::UNIMPLEMENTED;
    case grpc::StatusCode::INTERNAL: return StatusCode::INTERNAL;
    case grpc::StatusCode::UNAVAILABLE: return StatusCode::UNAVAILABLE;
    case grpc::StatusCode::DATA_LOSS: return StatusCode::DATA_LOSS;
    case grpc::StatusCode::UNAUTHENTICATED: return StatusCode::UNAUTHENTICATED;
    default: return StatusCode::UNKNOWN;
  }
}
#endif

}  // namespace

std::string HttpClientResponse::Header(std::string_view key) const {
  auto it = headers.find(ToLowerHeader(key));
  if (it != headers.end()) return it->second;
  return "";
}

RpcClient::RpcClient(std::string host, uint16_t port, uint32_t timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

Status RpcClient::Complete(const ChatCompletionRequest& req, ChatCompletionResponse* resp) {
  if (resp == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

#ifdef TESSERA_HAS_SYSTEM_GRPC
  std::string target = host_ + ":" + std::to_string(port_);
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto stub = tessera::gateway::v1::GatewayService::NewStub(channel);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_));

  tessera::gateway::v1::ChatCompletionRequest pb_req;
  pb_req.set_model(req.model);
  for (const auto& m : req.messages) {
    auto* pm = pb_req.add_messages();
    pm->set_role(m.role);
    pm->set_content(m.content);
    if (!m.name.empty()) pm->set_name(m.name);
  }
  if (req.temperature.has_value()) pb_req.set_temperature(*req.temperature);
  if (req.max_tokens.has_value()) pb_req.set_max_tokens(*req.max_tokens);
  pb_req.set_stream(req.stream);
  pb_req.set_correlation_id(req.correlation_id);
  pb_req.set_no_cache(req.no_cache);

  tessera::gateway::v1::ChatCompletionResponse pb_resp;
  grpc::Status gs = stub->ChatCompletion(&ctx, pb_req, &pb_resp);
  if (!gs.ok()) {
    return Status{FromGrpcStatusCode(gs.error_code()), gs.error_message()};
  }

  *resp = ChatCompletionResponse{};
  resp->id = pb_resp.id();
  resp->created = pb_resp.created();
  resp->model = pb_resp.model();
  resp->correlation_id = pb_resp.correlation_id();
  resp->cache_hit = pb_resp.cache_hit();
  resp->cache_tier = pb_resp.cache_tier();
  for (const auto& pc : pb_resp.choices()) {
    Choice c;
    c.index = pc.index();
    c.finish_reason = pc.finish_reason();
    c.message.role = pc.message().role();
    c.message.content = pc.message().content();
    c.message.name = pc.message().name();
    resp->choices.push_back(std::move(c));
  }
  resp->usage.prompt_tokens = pb_resp.usage().prompt_tokens();
  resp->usage.completion_tokens = pb_resp.usage().completion_tokens();
  resp->usage.total_tokens = pb_resp.usage().total_tokens();
  return Status::OK();
#else
  std::string conn_err;
  int fd = ConnectTcpHost(host_, port_, timeout_ms_, &conn_err);
  if (fd < 0) {
    return Status::Unavailable(conn_err);
  }

  JsonValue env = JsonValue::MakeObject();
  env.Set("method", "ChatCompletion");
  env.Set("request", json_codec::ChatRequestToJson(req));
  std::string payload = env.Dump();

  uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
  bool send_ok = SendAllSocket(fd, std::string_view(reinterpret_cast<const char*>(&net_len), 4)) &&
                 SendAllSocket(fd, payload);
  if (!send_ok) {
    ::close(fd);
    return Status::Unavailable("Failed sending RPC request to server");
  }

  uint32_t resp_net_len = 0;
  if (!RecvExactSocket(fd, &resp_net_len, 4)) {
    ::close(fd);
    return Status::Unavailable("Connection closed before RPC response header");
  }
  uint32_t resp_len = ntohl(resp_net_len);
  if (resp_len == 0 || resp_len > 16u * 1024u * 1024u) {
    ::close(fd);
    return Status::Internal("Invalid RPC response length");
  }

  std::string resp_payload(resp_len, '\0');
  if (!RecvExactSocket(fd, resp_payload.data(), resp_len)) {
    ::close(fd);
    return Status::Unavailable("Truncated RPC response payload");
  }
  ::close(fd);

  JsonValue out;
  std::string parse_err;
  if (!JsonValue::Parse(resp_payload, &out, &parse_err) || !out.IsObject()) {
    return Status::Internal("Invalid JSON in RPC response: " + parse_err);
  }

  StatusCode st_code = StatusCode::INTERNAL;
  if (const JsonValue* sc = out.Find("status_code"); sc && sc->IsNumber()) {
    st_code = static_cast<StatusCode>(sc->AsInt64());
  }
  std::string st_msg;
  if (const JsonValue* sm = out.Find("status_message"); sm && sm->IsString()) {
    st_msg = sm->AsString();
  }
  if (const JsonValue* r = out.Find("response"); r && r->IsObject() && resp != nullptr) {
    std::string resp_err;
    if (!json_codec::JsonToChatResponse(*r, resp, &resp_err)) {
      return Status::Internal("Invalid RPC response payload: " + resp_err);
    }
  }
  return Status{st_code, std::move(st_msg)};
#endif
}

Status RpcClient::CheckHealth(HealthStatus* out_status) {
  if (out_status == nullptr) {
    return Status::InvalidArgument("HealthStatus pointer cannot be null");
  }
  *out_status = HealthStatus::UNKNOWN;

#ifdef TESSERA_HAS_SYSTEM_GRPC
  std::string target = host_ + ":" + std::to_string(port_);
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto stub = tessera::gateway::v1::GatewayService::NewStub(channel);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_));

  tessera::gateway::v1::HealthCheckRequest pb_req;
  tessera::gateway::v1::HealthCheckResponse pb_resp;
  grpc::Status gs = stub->Check(&ctx, pb_req, &pb_resp);
  if (!gs.ok()) {
    return Status{FromGrpcStatusCode(gs.error_code()), gs.error_message()};
  }

  switch (pb_resp.status()) {
    case tessera::gateway::v1::HealthCheckResponse::SERVING:
      *out_status = HealthStatus::SERVING;
      break;
    case tessera::gateway::v1::HealthCheckResponse::NOT_SERVING:
      *out_status = HealthStatus::NOT_SERVING;
      break;
    default:
      *out_status = HealthStatus::UNKNOWN;
      break;
  }
  return Status::OK();
#else
  std::string conn_err;
  int fd = ConnectTcpHost(host_, port_, timeout_ms_, &conn_err);
  if (fd < 0) {
    return Status::Unavailable(conn_err);
  }

  JsonValue env = JsonValue::MakeObject();
  env.Set("method", "Check");
  env.Set("service", "");
  std::string payload = env.Dump();

  uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
  bool send_ok = SendAllSocket(fd, std::string_view(reinterpret_cast<const char*>(&net_len), 4)) &&
                 SendAllSocket(fd, payload);
  if (!send_ok) {
    ::close(fd);
    return Status::Unavailable("Failed sending health RPC to server");
  }

  uint32_t resp_net_len = 0;
  if (!RecvExactSocket(fd, &resp_net_len, 4)) {
    ::close(fd);
    return Status::Unavailable("Connection closed before health RPC response");
  }
  uint32_t resp_len = ntohl(resp_net_len);
  if (resp_len == 0 || resp_len > 65536u) {
    ::close(fd);
    return Status::Internal("Invalid health RPC response length");
  }

  std::string resp_payload(resp_len, '\0');
  if (!RecvExactSocket(fd, resp_payload.data(), resp_len)) {
    ::close(fd);
    return Status::Unavailable("Truncated health RPC response");
  }
  ::close(fd);

  JsonValue out;
  std::string parse_err;
  if (!JsonValue::Parse(resp_payload, &out, &parse_err) || !out.IsObject()) {
    return Status::Internal("Invalid JSON in health RPC response: " + parse_err);
  }

  if (const JsonValue* st = out.Find("status"); st && st->IsString()) {
    const std::string& s = st->AsString();
    if (s == "SERVING") *out_status = HealthStatus::SERVING;
    else if (s == "NOT_SERVING") *out_status = HealthStatus::NOT_SERVING;
    else *out_status = HealthStatus::UNKNOWN;
  }
  return Status::OK();
#endif
}

Status HttpRequestSync(
    std::string_view host,
    uint16_t port,
    std::string_view method,
    std::string_view path,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    HttpClientResponse* out_resp,
    uint32_t timeout_ms,
    const std::function<void(std::string_view)>& on_sse_event) {
  std::string conn_err;
  int fd = ConnectTcpHost(host, port, timeout_ms, &conn_err);
  if (fd < 0) {
    return Status::Unavailable(conn_err);
  }

  std::ostringstream req_oss;
  req_oss << method << " " << path << " HTTP/1.1\r\n";
  req_oss << "Host: " << host << ":" << port << "\r\n";
  req_oss << "Connection: close\r\n";
  if (!body.empty()) {
    req_oss << "Content-Length: " << body.size() << "\r\n";
  }
  for (const auto& [k, v] : headers) {
    req_oss << k << ": " << v << "\r\n";
  }
  req_oss << "\r\n";
  if (!body.empty()) {
    req_oss << body;
  }

  std::string raw_req = req_oss.str();
  if (!SendAllSocket(fd, raw_req)) {
    ::close(fd);
    return Status::Unavailable("Failed sending HTTP request");
  }

  HttpClientResponse parsed;
  std::string raw_buf;
  size_t header_end = std::string::npos;
  char chunk[4096];

  while (header_end == std::string::npos) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Status::Unavailable("Timed out reading HTTP response headers");
    }
    if (n == 0) break;
    raw_buf.append(chunk, static_cast<size_t>(n));
    header_end = raw_buf.find("\r\n\r\n");
  }

  if (header_end == std::string::npos) {
    ::close(fd);
    return Status::Unavailable("Malformed HTTP response (no header terminator)");
  }

  std::string_view hblock(raw_buf.data(), header_end);
  auto first_crlf = hblock.find("\r\n");
  std::string_view status_line = (first_crlf == std::string_view::npos)
                                     ? hblock
                                     : hblock.substr(0, first_crlf);
  auto sp1 = status_line.find(' ');
  if (sp1 != std::string_view::npos) {
    parsed.status_code = std::atoi(std::string(TrimAsciiView(status_line.substr(sp1 + 1, 3))).c_str());
  }

  size_t pos = (first_crlf == std::string_view::npos) ? hblock.size() : first_crlf + 2;
  while (pos < hblock.size()) {
    auto next_crlf = hblock.find("\r\n", pos);
    std::string_view line = (next_crlf == std::string_view::npos)
                                ? hblock.substr(pos)
                                : hblock.substr(pos, next_crlf - pos);
    pos = (next_crlf == std::string_view::npos) ? hblock.size() : next_crlf + 2;
    auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      parsed.headers[ToLowerHeader(TrimAsciiView(line.substr(0, colon)))] =
          std::string(TrimAsciiView(line.substr(colon + 1)));
    }
  }

  parsed.body = raw_buf.substr(header_end + 4);
  bool is_sse = parsed.Header("content-type").find("text/event-stream") != std::string::npos;

  size_t sse_cursor = 0;
  auto flush_sse_events = [&]() {
    if (!is_sse || !on_sse_event) return;
    while (true) {
      auto sep = parsed.body.find("\n\n", sse_cursor);
      if (sep == std::string::npos) break;
      std::string_view block(parsed.body.data() + sse_cursor, sep - sse_cursor);
      sse_cursor = sep + 2;
      if (block.rfind("data: ", 0) == 0) {
        on_sse_event(block.substr(6));
      } else if (block.rfind("data:", 0) == 0) {
        on_sse_event(TrimAsciiView(block.substr(5)));
      }
    }
  };
  flush_sse_events();

  while (true) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    parsed.body.append(chunk, static_cast<size_t>(n));
    flush_sse_events();
  }
  ::close(fd);

  if (out_resp != nullptr) {
    *out_resp = std::move(parsed);
  }
  return Status::OK();
}

}  // namespace tessera
