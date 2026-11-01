#include <gtest/gtest.h>
#include "tessera/server/health_service.hpp"

using namespace tessera;

TEST(HealthServiceTest, InitialStatusIsServing) {
  HealthService health;
  EXPECT_TRUE(health.IsServing());
  EXPECT_EQ(health.GetStatus(), HealthStatus::SERVING);
}

TEST(HealthServiceTest, TransitionToNotServing) {
  HealthService health;
  health.SetStatus(HealthStatus::NOT_SERVING);
  EXPECT_FALSE(health.IsServing());
  EXPECT_EQ(health.GetStatus(), HealthStatus::NOT_SERVING);
}

TEST(HealthServiceTest, TransitionToServing) {
  HealthService health(HealthStatus::NOT_SERVING);
  EXPECT_FALSE(health.IsServing());
  health.SetStatus(HealthStatus::SERVING);
  EXPECT_TRUE(health.IsServing());
  EXPECT_EQ(health.GetStatus(), HealthStatus::SERVING);
}

