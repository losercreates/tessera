#pragma once

#include <string>
#include <optional>
#include <vector>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <mutex>

namespace tessera {

class IRedisClient {
 public:
  virtual ~IRedisClient() = default;

  virtual bool Connect() = 0;
  virtual void Disconnect() = 0;
  [[nodiscard]] virtual bool IsConnected() const = 0;
  virtual bool Ping() = 0;

  virtual std::optional<std::string> Get(const std::string& key) = 0;
  virtual bool Set(const std::string& key, const std::string& value, uint32_t ttl_seconds = 0) = 0;
  virtual bool Del(const std::string& key) = 0;

  // Execute atomic rate limiter Lua script
  // Returns: {allowed (0 or 1), remaining_tokens, retry_after_ms}
  virtual std::vector<int64_t> EvalRateLimitLua(
      const std::string& key,
      double requested,
      double capacity,
      double fill_rate,
      uint32_t ttl_seconds) = 0;
};

struct MockRedisSharedState {
  mutable std::mutex mutex;
  bool connected{true};
  bool simulate_failure{false};

  struct ValueEntry {
    std::string data;
    std::chrono::steady_clock::time_point expires_at;
    bool has_ttl{false};
  };

  struct BucketEntry {
    double tokens{0.0};
    int64_t last_refill_ms{0};
  };

  std::unordered_map<std::string, ValueEntry> store;
  std::unordered_map<std::string, BucketEntry> ratelimit_store;
};

// Thread-safe in-memory MockRedisClient for unit testing
class MockRedisClient : public IRedisClient {
 public:
  MockRedisClient();
  explicit MockRedisClient(std::shared_ptr<MockRedisSharedState> shared_state);

  bool Connect() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->connected = true;
    return true;
  }
  void Disconnect() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->connected = false;
  }
  [[nodiscard]] bool IsConnected() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->connected && !state_->simulate_failure;
  }
  bool Ping() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->connected && !state_->simulate_failure;
  }

  std::optional<std::string> Get(const std::string& key) override;
  bool Set(const std::string& key, const std::string& value, uint32_t ttl_seconds = 0) override;
  bool Del(const std::string& key) override;

  std::vector<int64_t> EvalRateLimitLua(
      const std::string& key,
      double requested,
      double capacity,
      double fill_rate,
      uint32_t ttl_seconds) override;

  void Clear() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->store.clear();
    state_->ratelimit_store.clear();
  }

  void SetSimulateFailure(bool fail) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->simulate_failure = fail;
  }

  [[nodiscard]] std::shared_ptr<MockRedisSharedState> GetSharedState() const {
    return state_;
  }

 private:
  std::shared_ptr<MockRedisSharedState> state_;
};

// Real socket-based Redis client speaking Redis Serialization Protocol (RESP)
class SocketRedisClient : public IRedisClient {
 public:
  SocketRedisClient(std::string host, uint16_t port, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
  ~SocketRedisClient() override;

  bool Connect() override;
  void Disconnect() override;
  [[nodiscard]] bool IsConnected() const override { return connected_ && socket_fd_ >= 0; }
  bool Ping() override;

  std::optional<std::string> Get(const std::string& key) override;
  bool Set(const std::string& key, const std::string& value, uint32_t ttl_seconds = 0) override;
  bool Del(const std::string& key) override;

  std::vector<int64_t> EvalRateLimitLua(
      const std::string& key,
      double requested,
      double capacity,
      double fill_rate,
      uint32_t ttl_seconds) override;

 private:
  bool SendCommand(const std::vector<std::string>& args);
  std::string ReadLine();
  std::optional<std::string> ReadBulkString();

  std::string host_;
  uint16_t port_;
  std::chrono::milliseconds timeout_;
  int socket_fd_{-1};
  bool connected_{false};
  std::string read_buffer_;
};

}  // namespace tessera
