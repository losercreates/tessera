#include "tessera/cache/redis_client.hpp"
#include "tessera/common/logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cmath>
#include <sstream>

namespace tessera {

// ==========================================
// MockRedisClient Implementation
// ==========================================

MockRedisClient::MockRedisClient()
    : state_(std::make_shared<MockRedisSharedState>()) {}

MockRedisClient::MockRedisClient(std::shared_ptr<MockRedisSharedState> shared_state)
    : state_(std::move(shared_state)) {
  if (!state_) {
    state_ = std::make_shared<MockRedisSharedState>();
  }
}

std::optional<std::string> MockRedisClient::Get(const std::string& key) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->simulate_failure || !state_->connected) return std::nullopt;

  auto it = state_->store.find(key);
  if (it == state_->store.end()) {
    return std::nullopt;
  }

  if (it->second.has_ttl) {
    auto now = std::chrono::steady_clock::now();
    if (now >= it->second.expires_at) {
      state_->store.erase(it);
      return std::nullopt;
    }
  }

  return it->second.data;
}

bool MockRedisClient::Set(const std::string& key, const std::string& value, uint32_t ttl_seconds) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->simulate_failure || !state_->connected) return false;

  MockRedisSharedState::ValueEntry entry;
  entry.data = value;
  if (ttl_seconds > 0) {
    entry.has_ttl = true;
    entry.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
  } else {
    entry.has_ttl = false;
  }

  state_->store[key] = std::move(entry);
  return true;
}

bool MockRedisClient::Del(const std::string& key) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->simulate_failure || !state_->connected) return false;

  return state_->store.erase(key) > 0;
}

std::vector<int64_t> MockRedisClient::EvalRateLimitLua(
    const std::string& key,
    double requested,
    double capacity,
    double fill_rate,
    uint32_t /*ttl_seconds*/) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->simulate_failure || !state_->connected) {
    return {1, static_cast<int64_t>(capacity), 0};  // fail-open
  }

  auto now = std::chrono::system_clock::now();
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

  auto& bucket = state_->ratelimit_store[key];
  if (bucket.last_refill_ms == 0) {
    bucket.tokens = capacity;
    bucket.last_refill_ms = now_ms;
  } else {
    double elapsed_sec = std::max(0.0, static_cast<double>(now_ms - bucket.last_refill_ms) / 1000.0);
    bucket.tokens = std::min(capacity, bucket.tokens + (elapsed_sec * fill_rate));
    bucket.last_refill_ms = now_ms;
  }

  if (bucket.tokens >= requested) {
    bucket.tokens -= requested;
    return {1, static_cast<int64_t>(std::floor(bucket.tokens)), 0};
  } else {
    double needed = requested - bucket.tokens;
    int64_t retry_after_ms = static_cast<int64_t>(std::ceil((needed / fill_rate) * 1000.0));
    return {0, static_cast<int64_t>(std::floor(bucket.tokens)), retry_after_ms};
  }
}

// ==========================================
// SocketRedisClient (RESP Protocol) Implementation
// ==========================================

SocketRedisClient::SocketRedisClient(std::string host, uint16_t port, std::chrono::milliseconds timeout)
    : host_(std::move(host)), port_(port), timeout_(timeout) {}

SocketRedisClient::~SocketRedisClient() {
  Disconnect();
}

bool SocketRedisClient::Connect() {
  if (IsConnected()) return true;

  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    return false;
  }

  struct sockaddr_in serv_addr{};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port_);

  if (inet_pton(AF_INET, host_.c_str(), &serv_addr.sin_addr) <= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Set receive/send timeout
  struct timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout_.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout_.count() % 1000) * 1000);
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    connected_ = false;
    return false;
  }

  connected_ = true;
  read_buffer_.clear();
  return true;
}

void SocketRedisClient::Disconnect() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  connected_ = false;
  read_buffer_.clear();
}

bool SocketRedisClient::Ping() {
  if (!Connect()) return false;
  if (!SendCommand({"PING"})) return false;
  std::string line = ReadLine();
  return line == "+PONG";
}

bool SocketRedisClient::SendCommand(const std::vector<std::string>& args) {
  if (!IsConnected() && !Connect()) return false;

  std::ostringstream oss;
  oss << "*" << args.size() << "\r\n";
  for (const auto& arg : args) {
    oss << "$" << arg.size() << "\r\n" << arg << "\r\n";
  }

  std::string cmd = oss.str();
  ssize_t sent = send(socket_fd_, cmd.data(), cmd.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<size_t>(sent) != cmd.size()) {
    Disconnect();
    return false;
  }
  return true;
}

std::string SocketRedisClient::ReadLine() {
  std::string line;
  char ch = 0;
  while (true) {
    ssize_t n = recv(socket_fd_, &ch, 1, 0);
    if (n <= 0) {
      Disconnect();
      return "";
    }
    if (ch == '\r') {
      char next = 0;
      recv(socket_fd_, &next, 1, 0);  // Consume '\n'
      break;
    }
    line += ch;
  }
  return line;
}

std::optional<std::string> SocketRedisClient::ReadBulkString() {
  std::string header = ReadLine();
  if (header.empty() || header[0] != '$') {
    return std::nullopt;
  }

  int len = std::stoi(header.substr(1));
  if (len == -1) {
    return std::nullopt;  // Key does not exist
  }

  std::string result(static_cast<size_t>(len), '\0');
  size_t bytes_read = 0;
  while (bytes_read < static_cast<size_t>(len)) {
    ssize_t n = recv(socket_fd_, &result[bytes_read], static_cast<size_t>(len) - bytes_read, 0);
    if (n <= 0) {
      Disconnect();
      return std::nullopt;
    }
    bytes_read += static_cast<size_t>(n);
  }

  char crlf[2];
  recv(socket_fd_, crlf, 2, 0);  // Consume trailing \r\n
  return result;
}

std::optional<std::string> SocketRedisClient::Get(const std::string& key) {
  if (!SendCommand({"GET", key})) return std::nullopt;
  return ReadBulkString();
}

bool SocketRedisClient::Set(const std::string& key, const std::string& value, uint32_t ttl_seconds) {
  std::vector<std::string> cmd;
  if (ttl_seconds > 0) {
    cmd = {"SET", key, value, "EX", std::to_string(ttl_seconds)};
  } else {
    cmd = {"SET", key, value};
  }

  if (!SendCommand(cmd)) return false;
  std::string resp = ReadLine();
  return resp == "+OK";
}

bool SocketRedisClient::Del(const std::string& key) {
  if (!SendCommand({"DEL", key})) return false;
  std::string resp = ReadLine();
  return !resp.empty() && resp[0] == ':';
}

std::vector<int64_t> SocketRedisClient::EvalRateLimitLua(
    const std::string& key,
    double requested,
    double capacity,
    double fill_rate,
    uint32_t ttl_seconds) {
  static const std::string lua_script =
      "local key = KEYS[1]\n"
      "local requested = tonumber(ARGV[1])\n"
      "local capacity = tonumber(ARGV[2])\n"
      "local fill_rate = tonumber(ARGV[3])\n"
      "local ttl = tonumber(ARGV[4])\n"
      "local time = redis.call('TIME')\n"
      "local now_ms = (tonumber(time[1]) * 1000) + math.floor(tonumber(time[2]) / 1000)\n"
      "local data = redis.call('HMGET', key, 'tokens', 'last_refill_ms')\n"
      "local current_tokens = capacity\n"
      "local last_refill_ms = now_ms\n"
      "if data[1] and data[2] then\n"
      "    current_tokens = tonumber(data[1])\n"
      "    last_refill_ms = tonumber(data[2])\n"
      "    local elapsed_sec = math.max(0, (now_ms - last_refill_ms) / 1000.0)\n"
      "    current_tokens = math.min(capacity, current_tokens + (elapsed_sec * fill_rate))\n"
      "end\n"
      "if current_tokens >= requested then\n"
      "    current_tokens = current_tokens - requested\n"
      "    redis.call('HMSET', key, 'tokens', tostring(current_tokens), 'last_refill_ms', tostring(now_ms))\n"
      "    redis.call('EXPIRE', key, ttl)\n"
      "    return { 1, math.floor(current_tokens), 0 }\n"
      "else\n"
      "    local needed = requested - current_tokens\n"
      "    local retry_after_ms = math.ceil((needed / fill_rate) * 1000)\n"
      "    redis.call('HMSET', key, 'tokens', tostring(current_tokens), 'last_refill_ms', tostring(now_ms))\n"
      "    redis.call('EXPIRE', key, ttl)\n"
      "    return { 0, math.floor(current_tokens), retry_after_ms }\n"
      "end";

  std::vector<std::string> cmd = {
      "EVAL", lua_script, "1", key,
      std::to_string(requested),
      std::to_string(capacity),
      std::to_string(fill_rate),
      std::to_string(ttl_seconds)
  };

  if (!SendCommand(cmd)) {
    return {1, static_cast<int64_t>(capacity), 0};  // fail-open on network error
  }

  // Parse RESP Array response: *3\r\n:1\r\n:29\r\n:0\r\n
  std::string header = ReadLine();
  if (header.empty() || header[0] != '*') {
    return {1, static_cast<int64_t>(capacity), 0};
  }

  int count = std::stoi(header.substr(1));
  std::vector<int64_t> results;
  for (int i = 0; i < count; ++i) {
    std::string item = ReadLine();
    if (!item.empty() && item[0] == ':') {
      results.push_back(std::stoll(item.substr(1)));
    } else {
      results.push_back(0);
    }
  }

  if (results.size() < 3) {
    return {1, static_cast<int64_t>(capacity), 0};
  }

  return results;
}

}  // namespace tessera
