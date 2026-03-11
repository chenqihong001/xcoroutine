#include "xcoro/when_all.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(WhenAllTest, ReturnsTupleOfAllResults) {
  auto first = []() -> task<int> {
    co_return 3;
  };

  auto second = []() -> task<std::string> {
    co_return std::string{"ok"};
  };

  auto result = sync_wait(when_all(first(), second()));
  EXPECT_EQ(std::get<0>(result), 3);
  EXPECT_EQ(std::get<1>(result), "ok");
}

TEST(WhenAllTest, RethrowsTaskException) {
  auto good = []() -> task<int> {
    co_return 1;
  };

  auto bad = []() -> task<int> {
    throw std::runtime_error("boom");
    co_return 0;
  };

  EXPECT_THROW(sync_wait(when_all(good(), bad())), std::runtime_error);
}

TEST(WhenAllTest, CancellationOverloadCancelsOtherTasksOnFailure) {
  cancellation_source source;
  semaphore sem(0);
  std::atomic<bool> cancelled{false};

  auto failing = []() -> task<int> {
    throw std::runtime_error("failure");
    co_return 0;
  };

  auto blocked = [&]() -> task<int> {
    try {
      co_await sem.acquire(source.token());
    } catch (const operation_cancelled&) {
      cancelled.store(true, std::memory_order_release);
      co_return -1;
    }
    co_return 1;
  };

  EXPECT_THROW(sync_wait(when_all(source, failing(), blocked())), std::runtime_error);
  EXPECT_TRUE(source.is_cancellation_requested());

  for (int i = 0; i < 1024 && !cancelled.load(std::memory_order_acquire); ++i) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(cancelled.load(std::memory_order_acquire));
}

