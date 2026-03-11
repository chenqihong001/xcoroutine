#include "xcoro/when_any.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(WhenAnyTest, ReturnsFirstCompletedResult) {
  manual_reset_event gate;

  auto fast = []() -> task<int> {
    co_return 7;
  };

  auto slow = [&]() -> task<int> {
    co_await gate;
    co_return 42;
  };

  auto result = sync_wait(when_any(fast(), slow()));
  EXPECT_EQ(result.index, 0u);
  EXPECT_EQ(result.get<0>(), 7);

  gate.set();
}

TEST(WhenAnyTest, RangeOverloadReturnsFirstCompletedResult) {
  manual_reset_event gate;

  std::vector<task<int>> tasks;
  tasks.push_back([&]() -> task<int> {
    co_await gate;
    co_return 1;
  }());
  tasks.push_back([]() -> task<int> {
    co_return 2;
  }());

  auto result = sync_wait(when_any(std::move(tasks)));
  EXPECT_EQ(result.index, 1u);
  EXPECT_EQ(result.get(), 2);

  gate.set();
}

TEST(WhenAnyTest, CancellationOverloadCancelsLosers) {
  cancellation_source source;
  semaphore sem(0);
  std::atomic<bool> loser_cancelled{false};

  auto fast = []() -> task<int> {
    co_return 10;
  };

  auto slow = [&]() -> task<int> {
    try {
      co_await sem.acquire(source.token());
    } catch (const operation_cancelled&) {
      loser_cancelled.store(true, std::memory_order_release);
      co_return -1;
    }
    co_return 20;
  };

  auto result = sync_wait(when_any(source, fast(), slow()));

  EXPECT_EQ(result.index, 0u);
  EXPECT_EQ(result.get<0>(), 10);
  EXPECT_TRUE(source.is_cancellation_requested());

  for (int i = 0; i < 1024 && !loser_cancelled.load(std::memory_order_acquire); ++i) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(loser_cancelled.load(std::memory_order_acquire));
}

