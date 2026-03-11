#include "xcoro/latch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(LatchTest, BasicCountDownAndTryWait) {
  latch l(2);
  EXPECT_FALSE(l.try_wait());
  l.count_down();
  EXPECT_FALSE(l.try_wait());
  l.count_down();
  EXPECT_TRUE(l.try_wait());
}

TEST(LatchTest, WaitResumesWhenCountReachesZero) {
  latch l(2);
  std::atomic<bool> waiting{false};
  std::atomic<bool> passed{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    co_await l.wait();
    passed.store(true, std::memory_order_release);
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  l.count_down();
  EXPECT_FALSE(passed.load(std::memory_order_acquire));
  l.count_down();
  fut.get();

  EXPECT_TRUE(passed.load(std::memory_order_acquire));
  EXPECT_TRUE(l.try_wait());
}

TEST(LatchTest, WaitingCoroutineResumedByCancellation) {
  latch l(1);
  cancellation_source source;

  std::atomic<bool> waiting{false};
  std::atomic<bool> cancelled{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    try {
      co_await l.wait(source.token());
    } catch (const operation_cancelled&) {
      cancelled.store(true, std::memory_order_release);
    }
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  source.request_cancellation();
  fut.get();

  EXPECT_TRUE(cancelled.load(std::memory_order_acquire));
  EXPECT_FALSE(l.try_wait());
  l.count_down();
  EXPECT_TRUE(l.try_wait());
}

TEST(LatchTest, AlreadyCancelledTokenThrowsImmediately) {
  latch l(1);
  cancellation_source source;
  source.request_cancellation();

  auto waiter = [&]() -> task<> {
    co_await l.wait(source.token());
  };

  EXPECT_THROW(sync_wait(waiter()), operation_cancelled);
}

