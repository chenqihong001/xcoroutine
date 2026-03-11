#include "xcoro/mutex.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(MutexTest, BasicTryLockUnlock) {
  mutex m;
  EXPECT_TRUE(m.try_lock());
  EXPECT_FALSE(m.try_lock());
  m.unlock();
  EXPECT_TRUE(m.try_lock());
  m.unlock();
}

TEST(MutexTest, WaitingCoroutineResumedByUnlock) {
  mutex m;
  ASSERT_TRUE(m.try_lock());

  std::atomic<bool> waiting{false};
  std::atomic<bool> acquired{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    co_await m.lock();
    acquired.store(true, std::memory_order_release);
    m.unlock();
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  EXPECT_FALSE(acquired.load(std::memory_order_acquire));
  m.unlock();
  fut.get();

  EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

TEST(MutexTest, WaitingCoroutineResumedByCancellation) {
  mutex m;
  ASSERT_TRUE(m.try_lock());

  cancellation_source source;
  std::atomic<bool> waiting{false};
  std::atomic<bool> cancelled{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    try {
      co_await m.lock(source.token());
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
  EXPECT_FALSE(m.try_lock());
  m.unlock();
  EXPECT_TRUE(m.try_lock());
  m.unlock();
}

