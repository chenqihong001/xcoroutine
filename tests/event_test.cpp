#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/manual_reset_event.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(EventTest, SetResumesSingleWaiter) {
  manual_reset_event ev;
  std::atomic<bool> waiting{false};
  std::atomic<bool> resumed{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    co_await ev;
    resumed.store(true, std::memory_order_release);
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  EXPECT_FALSE(resumed.load(std::memory_order_acquire));
  ev.set();
  fut.get();
  EXPECT_TRUE(resumed.load(std::memory_order_acquire));
}

TEST(EventTest, SetIsStickyForFutureWaiters) {
  manual_reset_event ev;
  ev.set();
  EXPECT_TRUE(ev.is_set());

  std::atomic<bool> resumed{false};
  auto waiter = [&]() -> task<> {
    co_await ev;
    resumed.store(true, std::memory_order_release);
  };

  sync_wait(waiter());
  EXPECT_TRUE(resumed.load(std::memory_order_acquire));
}

TEST(EventTest, SetResumesAllCurrentWaiters) {
  manual_reset_event ev;
  std::atomic<int> waiting_count{0};
  std::atomic<int> resumed_count{0};

  auto waiter = [&]() -> task<> {
    waiting_count.fetch_add(1, std::memory_order_acq_rel);
    co_await ev;
    resumed_count.fetch_add(1, std::memory_order_acq_rel);
  };

  auto fut1 = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });
  auto fut2 = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (waiting_count.load(std::memory_order_acquire) < 2) {
    std::this_thread::yield();
  }

  ev.set();
  fut1.get();
  fut2.get();

  EXPECT_EQ(resumed_count.load(std::memory_order_acquire), 2);
}
