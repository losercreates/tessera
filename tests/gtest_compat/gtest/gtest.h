#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <cmath>
#include <memory>

namespace testing {

class Test {
 public:
  virtual ~Test() = default;
  virtual void SetUp() {}
  virtual void TearDown() {}
  virtual void TestBody() = 0;
};

struct TestInfo {
  std::string suite_name;
  std::string test_name;
  std::function<std::unique_ptr<Test>()> factory;
};

class UnitTest {
 public:
  static UnitTest* GetInstance() {
    static UnitTest instance;
    return &instance;
  }

  void RegisterTest(std::string suite, std::string name, std::function<std::unique_ptr<Test>()> factory) {
    tests_.push_back({std::move(suite), std::move(name), std::move(factory)});
  }

  int RunAllTests() {
    int passed = 0;
    int failed = 0;
    std::cout << "[==========] Running " << tests_.size() << " tests.\n";

    for (const auto& test_info : tests_) {
      std::cout << "[ RUN      ] " << test_info.suite_name << "." << test_info.test_name << "\n";
      auto test_instance = test_info.factory();
      current_test_failed_ = false;

      try {
        test_instance->SetUp();
        test_instance->TestBody();
        test_instance->TearDown();
      } catch (const std::exception& e) {
        current_test_failed_ = true;
        std::cerr << "      Exception caught: " << e.what() << "\n";
      } catch (...) {
        current_test_failed_ = true;
        std::cerr << "      Unknown exception caught.\n";
      }

      if (!current_test_failed_) {
        std::cout << "[       OK ] " << test_info.suite_name << "." << test_info.test_name << "\n";
        passed++;
      } else {
        std::cout << "[  FAILED  ] " << test_info.suite_name << "." << test_info.test_name << "\n";
        failed++;
      }
    }

    std::cout << "[==========] " << tests_.size() << " tests ran.\n";
    std::cout << "[  PASSED  ] " << passed << " tests.\n";
    if (failed > 0) {
      std::cout << "[  FAILED  ] " << failed << " tests.\n";
      return 1;
    }
    return 0;
  }

  void MarkCurrentFailed() {
    current_test_failed_ = true;
  }

 private:
  std::vector<TestInfo> tests_;
  bool current_test_failed_{false};
};

inline void InitGoogleTest(int*, char**) {}

}  // namespace testing

#define TEST(test_suite_name, test_name)                                                                   \
  class test_suite_name##_##test_name##_Test : public ::testing::Test {                                     \
   public:                                                                                                  \
    void TestBody() override;                                                                               \
  };                                                                                                        \
  static bool test_suite_name##_##test_name##_registered = []() {                                           \
    ::testing::UnitTest::GetInstance()->RegisterTest(                                                       \
        #test_suite_name, #test_name, []() { return std::make_unique<test_suite_name##_##test_name##_Test>(); }); \
    return true;                                                                                            \
  }();                                                                                                      \
  void test_suite_name##_##test_name##_Test::TestBody()

#define TEST_F(fixture_class, test_name)                                                                   \
  class fixture_class##_##test_name##_Test : public fixture_class {                                        \
   public:                                                                                                  \
    void TestBody() override;                                                                               \
  };                                                                                                        \
  static bool fixture_class##_##test_name##_registered = []() {                                             \
    ::testing::UnitTest::GetInstance()->RegisterTest(                                                       \
        #fixture_class, #test_name, []() { return std::make_unique<fixture_class##_##test_name##_Test>(); }); \
    return true;                                                                                            \
  }();                                                                                                      \
  void fixture_class##_##test_name##_Test::TestBody()

#define EXPECT_TRUE(condition)                                                                             \
  do {                                                                                                      \
    if (!(condition)) {                                                                                     \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_TRUE(" #condition ") failed.\n";      \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_FALSE(condition)                                                                            \
  do {                                                                                                      \
    if (condition) {                                                                                        \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_FALSE(" #condition ") failed.\n";     \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_EQ(val1, val2)                                                                              \
  do {                                                                                                      \
    if (!((val1) == (val2))) {                                                                              \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_EQ(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " vs " << (val2) << "\n";                                                     \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_NE(val1, val2)                                                                              \
  do {                                                                                                      \
    if ((val1) == (val2)) {                                                                                 \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_NE(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " == " << (val2) << "\n";                                                     \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_LT(val1, val2)                                                                              \
  do {                                                                                                      \
    if (!((val1) < (val2))) {                                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_LT(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " >= " << (val2) << "\n";                                                     \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_LE(val1, val2)                                                                              \
  do {                                                                                                      \
    if (!((val1) <= (val2))) {                                                                              \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_LE(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " > " << (val2) << "\n";                                                      \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_GT(val1, val2)                                                                              \
  do {                                                                                                      \
    if (!((val1) > (val2))) {                                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_GT(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " <= " << (val2) << "\n";                                                     \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define EXPECT_GE(val1, val2)                                                                              \
  do {                                                                                                      \
    if (!((val1) >= (val2))) {                                                                              \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: EXPECT_GE(" #val1 ", " #val2 ") failed: "   \
                << (val1) << " < " << (val2) << "\n";                                                      \
      ::testing::UnitTest::GetInstance()->MarkCurrentFailed();                                             \
    }                                                                                                       \
  } while (0)

#define ASSERT_TRUE(cond) EXPECT_TRUE(cond)
#define ASSERT_FALSE(cond) EXPECT_FALSE(cond)
#define ASSERT_EQ(v1, v2) EXPECT_EQ(v1, v2)
#define ASSERT_NE(v1, v2) EXPECT_NE(v1, v2)

inline int RUN_ALL_TESTS() {
  return ::testing::UnitTest::GetInstance()->RunAllTests();
}
