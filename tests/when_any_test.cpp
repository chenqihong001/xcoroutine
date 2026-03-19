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
  EXPECT_EQ(result.active_index(), 0u);
  EXPECT_EQ(result.get<0>(), 7);

  gate.set();
}

TEST(WhenAnyTest, RangeOverloadReturnsFirstCompletedResult) {
  manual_reset_event gate;
  auto gated_task = [](manual_reset_event& event) -> task<int> {
    co_await event;
    co_return 1;
  };

  std::vector<task<int>> tasks;
  tasks.push_back(gated_task(gate));
  tasks.push_back([]() -> task<int> {
    co_return 2;
  }());

  auto result = sync_wait(when_any(std::move(tasks)));
  EXPECT_EQ(result.get(), 2);

  gate.set();
}

TEST(WhenAnyTest, CancellationOverloadCancelsAllCancellationAwareLosers) {
  cancellation_source source;
  semaphore sem(0);
  std::atomic<int> cancelled_losers{0};

  auto fast = [](cancellation_token token) -> task<int> {
    EXPECT_FALSE(token.is_cancellation_requested());
    co_return 10;
  };

  auto wait_or_cancel = [&](cancellation_token token) -> task<int> {
    try {
      co_await sem.acquire(token);
    } catch (const operation_cancelled&) {
      cancelled_losers.fetch_add(1, std::memory_order_acq_rel);
      co_return -1;
    }
    co_return 20;
  };

  auto result = sync_wait(
      when_any(source, fast(source.token()), wait_or_cancel(source.token()),
               wait_or_cancel(source.token())));

  EXPECT_TRUE(result.holds<0>());
  EXPECT_EQ(result.get<0>(), 10);
  EXPECT_TRUE(source.is_cancellation_requested());

  for (int i = 0;
       i < 2048 && cancelled_losers.load(std::memory_order_acquire) < 2;
       ++i) {
    std::this_thread::yield();
  }
  EXPECT_EQ(cancelled_losers.load(std::memory_order_acquire), 2);
}

TEST(WhenAnyTest, CancellationRangeOverloadCancelsAllCancellationAwareLosers) {
  cancellation_source source;
  semaphore sem(0);
  std::atomic<int> cancelled_losers{0};

  auto wait_or_cancel = [&](cancellation_token token, int value) -> task<int> {
    try {
      co_await sem.acquire(token);
    } catch (const operation_cancelled&) {
      cancelled_losers.fetch_add(1, std::memory_order_acq_rel);
      co_return -1;
    }
    co_return value;
  };

  std::vector<task<int>> tasks;
  tasks.push_back([](cancellation_token token) -> task<int> {
    EXPECT_FALSE(token.is_cancellation_requested());
    co_return 99;
  }(source.token()));
  tasks.push_back(wait_or_cancel(source.token(), 1));
  tasks.push_back(wait_or_cancel(source.token(), 2));

  auto result = sync_wait(when_any(source, std::move(tasks)));

  EXPECT_EQ(result.get(), 99);
  EXPECT_TRUE(source.is_cancellation_requested());

  for (int i = 0;
       i < 2048 && cancelled_losers.load(std::memory_order_acquire) < 2;
       ++i) {
    std::this_thread::yield();
  }
  EXPECT_EQ(cancelled_losers.load(std::memory_order_acquire), 2);
}
