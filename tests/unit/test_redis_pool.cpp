#include <gtest/gtest.h>
#include "tessera/cache/redis_pool.hpp"
#include <thread>
#include <vector>

using namespace tessera;

TEST(RedisConnectionPoolTest, AcquireAndRelease) {
  RedisPoolConfig config;
  config.min_connections = 2;
  config.max_connections = 5;
  config.acquire_timeout = std::chrono::milliseconds(50);

  auto mock = std::make_shared<MockRedisClient>();
  RedisConnectionPool pool(config, mock);

  EXPECT_TRUE(pool.IsHealthy());

  {
    PooledRedisConnection conn(pool);
    EXPECT_TRUE(conn.IsValid());
    EXPECT_TRUE(conn->Ping());
  }
  // Connection returned upon scope exit
  EXPECT_TRUE(pool.IsHealthy());
}

TEST(RedisConnectionPoolTest, PoolExhaustionTimeout) {
  RedisPoolConfig config;
  config.min_connections = 1;
  config.max_connections = 2;
  config.acquire_timeout = std::chrono::milliseconds(50);

  auto mock = std::make_shared<MockRedisClient>();
  RedisConnectionPool pool(config, mock);

  auto conn1 = pool.Acquire();
  auto conn2 = pool.Acquire();
  EXPECT_NE(conn1, nullptr);
  EXPECT_NE(conn2, nullptr);

  // 3rd acquire must time out since max_connections is 2
  auto conn3 = pool.Acquire();
  EXPECT_EQ(conn3, nullptr);

  // Releasing conn1 allows acquire to succeed
  pool.Release(std::move(conn1));
  auto conn4 = pool.Acquire();
  EXPECT_NE(conn4, nullptr);

  pool.Release(std::move(conn2));
  pool.Release(std::move(conn4));
}

TEST(RedisConnectionPoolTest, HealthStateTransitions) {
  RedisPoolConfig config;
  auto mock = std::make_shared<MockRedisClient>();
  RedisConnectionPool pool(config, mock);

  EXPECT_TRUE(pool.IsHealthy());
  pool.SetHealthy(false);
  EXPECT_FALSE(pool.IsHealthy());
  pool.SetHealthy(true);
  EXPECT_TRUE(pool.IsHealthy());
}

TEST(RedisConnectionPoolTest, ConcurrentPoolAcquisitions) {
  RedisPoolConfig config;
  config.min_connections = 4;
  config.max_connections = 10;
  config.acquire_timeout = std::chrono::milliseconds(500);

  auto mock = std::make_shared<MockRedisClient>();
  RedisConnectionPool pool(config, mock);

  constexpr size_t kThreads = 20;
  std::vector<std::thread> workers;
  std::atomic<size_t> successful_ops{0};

  for (size_t i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      for (int j = 0; j < 10; ++j) {
        PooledRedisConnection conn(pool);
        if (conn.IsValid()) {
          conn->Ping();
          successful_ops.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  EXPECT_GT(successful_ops.load(), 0u);
}

