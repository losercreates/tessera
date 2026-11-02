#pragma once

#include "tessera/cache/redis_client.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>

namespace tessera {

struct RedisPoolConfig {
  std::string host{"127.0.0.1"};
  uint16_t port{6379};
  size_t min_connections{2};
  size_t max_connections{16};
  std::chrono::milliseconds acquire_timeout{1000};
  std::chrono::milliseconds reconnect_interval{5000};
};

class RedisConnectionPool {
 public:
  explicit RedisConnectionPool(RedisPoolConfig config, std::shared_ptr<IRedisClient> mock_client = nullptr);
  ~RedisConnectionPool();

  // Acquire a client from the pool (blocks up to acquire_timeout)
  std::unique_ptr<IRedisClient> Acquire();

  // Return a client back to the pool
  void Release(std::unique_ptr<IRedisClient> client);

  // Check overall health
  [[nodiscard]] bool IsHealthy() const noexcept {
    return is_healthy_.load(std::memory_order_acquire);
  }

  void SetHealthy(bool healthy) noexcept {
    is_healthy_.store(healthy, std::memory_order_release);
  }

  [[nodiscard]] size_t AvailableCount() const;

 private:
  std::unique_ptr<IRedisClient> CreateClient();

  RedisPoolConfig config_;
  std::shared_ptr<IRedisClient> mock_client_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::unique_ptr<IRedisClient>> pool_;
  size_t total_created_{0};
  std::atomic<bool> is_healthy_{true};
};

// RAII Guard for pooled connections
class PooledRedisConnection {
 public:
  explicit PooledRedisConnection(RedisConnectionPool& pool)
      : pool_(pool), client_(pool.Acquire()) {}

  ~PooledRedisConnection() {
    if (client_) {
      pool_.Release(std::move(client_));
    }
  }

  IRedisClient* operator->() noexcept { return client_.get(); }
  IRedisClient* get() noexcept { return client_.get(); }
  [[nodiscard]] bool IsValid() const noexcept { return client_ != nullptr; }

 private:
  RedisConnectionPool& pool_;
  std::unique_ptr<IRedisClient> client_;
};

}  // namespace tessera

