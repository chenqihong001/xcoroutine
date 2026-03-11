#include "xcoro/semaphore.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
using namespace xcoro;

TEST(SemaphoreTest, BasicAcquireRelease) {
  semaphore sem(2);
  EXPECT_TRUE(sem.try_acquire());
  EXPECT_TRUE(sem.try_acquire());
  EXPECT_FALSE(sem.try_acquire());
  sem.release(1);
  EXPECT_TRUE(sem.try_acquire());
}

TEST(SemaphoreTest, CountingSemaphoreMaxMatchesTemplateArg) {
  EXPECT_EQ((counting_semaphore<3>::max()), 3);
}

TEST(SemaphoreTest, BinarySemaphoreSinglePermit) {
  binary_semaphore sem(1);
  EXPECT_TRUE(sem.try_acquire());
  EXPECT_FALSE(sem.try_acquire());
  sem.release();
  EXPECT_TRUE(sem.try_acquire());
}

TEST(SemaphoreTest, AcquireWithAlreadyCancelledTokenThrows) {
  semaphore sem(1);
  cancellation_source source;
  source.request_cancellation();

  auto waiter = [&]() -> task<> {
    co_await sem.acquire(source.token());
  };

  EXPECT_THROW(sync_wait(waiter()), operation_cancelled);
}

TEST(SemaphoreTest, WaitingCoroutineIsResumedByRelease) {
  semaphore sem(0);
  std::atomic<bool> waiting{false};
  std::atomic<bool> acquired{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    co_await sem.acquire();
    acquired.store(true, std::memory_order_release);
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  sem.release();
  fut.get();

  EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

TEST(SemaphoreTest, WaitingCoroutineIsResumedByCancellation) {
  semaphore sem(0);
  cancellation_source source;
  std::atomic<bool> waiting{false};
  std::atomic<bool> cancelled{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    try {
      co_await sem.acquire(source.token());
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
}

TEST(SemaphoreTest, CancelledWaiterDoesNotConsumeFuturePermit) {
  semaphore sem(0);
  cancellation_source source;
  std::atomic<bool> waiting{false};
  std::atomic<bool> cancelled{false};

  auto waiter = [&]() -> task<> {
    waiting.store(true, std::memory_order_release);
    try {
      co_await sem.acquire(source.token());
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
  EXPECT_FALSE(sem.try_acquire());
  sem.release();
  EXPECT_TRUE(sem.try_acquire());
}
