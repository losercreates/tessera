#include "tessera/cache/redis_pool.hpp"
#include "tessera/common/logger.hpp"

namespace tessera {

RedisConnectionPool::RedisConnectionPool(RedisPoolConfig config, std::shared_ptr<IRedisClient> mock_client)
    : config_(std::move(config)), mock_client_(std::move(mock_client)) {
  for (size_t i = 0; i < config_.min_connections; ++i) {
    auto client = CreateClient();
    if (client) {
      pool_.push(std::move(client));
      total_created_++;
    }
  }
}

RedisConnectionPool::~RedisConnectionPool() {
  std::lock_guard<std::mutex> lock(mutex_);
  while (!pool_.empty()) {
    pool_.pop();
  }
}

std::unique_ptr<IRedisClient> RedisConnectionPool::CreateClient() {
  if (mock_client_ != nullptr) {
    auto* mock_ptr = dynamic_cast<MockRedisClient*>(mock_client_.get());
    if (mock_ptr != nullptr) {
      return std::make_unique<MockRedisClient>(mock_ptr->GetSharedState());
    }
    return std::make_unique<MockRedisClient>();
  }

  auto client = std::make_unique<SocketRedisClient>(config_.host, config_.port);
  if (!client->Connect()) {
    TESSERA_LOG_WARN("Failed to establish Redis connection to {}:{}", config_.host, config_.port);
    SetHealthy(false);
  } else {
    SetHealthy(true);
  }
  return client;
}

std::unique_ptr<IRedisClient> RedisConnectionPool::Acquire() {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!pool_.empty()) {
    auto client = std::move(pool_.front());
    pool_.pop();
    return client;
  }

  if (total_created_ < config_.max_connections) {
    total_created_++;
    lock.unlock();
    return CreateClient();
  }

  // Wait for an available connection up to acquire_timeout
  if (cv_.wait_for(lock, config_.acquire_timeout, [this]() { return !pool_.empty(); })) {
    auto client = std::move(pool_.front());
    pool_.pop();
    return client;
  }

  TESSERA_LOG_WARN("RedisConnectionPool: Connection acquisition timed out after {}ms",
                   config_.acquire_timeout.count());
  return nullptr;
}

void RedisConnectionPool::Release(std::unique_ptr<IRedisClient> client) {
  if (!client) return;

  std::lock_guard<std::mutex> lock(mutex_);
  pool_.push(std::move(client));
  cv_.notify_one();
}

size_t RedisConnectionPool::AvailableCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pool_.size();
}

}  // namespace tessera
