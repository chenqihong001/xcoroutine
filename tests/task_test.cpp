#include "xcoroutine/task.hpp"
#include <gtest/gtest.h>
using namespace xcoro;

TEST(TaskTest, BasicCreationAndExecution) {
  auto simple_task = []() -> task<int> { co_return 42; };

  //   EXPECT_EQ(result, 42);
}