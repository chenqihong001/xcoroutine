#include "xcoro/condition_variable.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

using namespace xcoro;

TEST(ConditionVariableTest, NotifyOneResumesWaiter) {
  mutex m;
  condition_variable cv;

  bool ready = false;
  std::atomic<bool> waiting{false};
  std::atomic<bool> done{false};
  std::atomic<bool> lock_reacquired{false};

  auto waiter = [&]() -> task<> {
    co_await m.lock();
    while (!ready) {
      waiting.store(true, std::memory_order_release);
      co_await cv.wait(m);
    }

    const bool owned = !m.try_lock();
    if (!owned) {
      m.unlock();
    }
    lock_reacquired.store(owned, std::memory_order_release);
    if (owned) {
      m.unlock();
    }
    done.store(true, std::memory_order_release);
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  while (!m.try_lock()) {
    std::this_thread::yield();
  }
  ready = true;
  m.unlock();

  for (int i = 0; i < 128 && !done.load(std::memory_order_acquire); ++i) {
    cv.notify_one();
    std::this_thread::yield();
  }

  fut.get();
  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(lock_reacquired.load(std::memory_order_acquire));
}

TEST(ConditionVariableTest, CancellationResumesWaiterAndKeepsLockDiscipline) {
  mutex m;
  condition_variable cv;
  cancellation_source source;

  std::atomic<bool> waiting{false};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> lock_reacquired{false};

  auto waiter = [&]() -> task<> {
    co_await m.lock();
    waiting.store(true, std::memory_order_release);
    try {
      co_await cv.wait(m, source.token());
    } catch (const operation_cancelled&) {
      cancelled.store(true, std::memory_order_release);
      const bool owned = !m.try_lock();
      if (!owned) {
        m.unlock();
      }
      lock_reacquired.store(owned, std::memory_order_release);
      if (owned) {
        m.unlock();
      }
      co_return;
    }

    m.unlock();
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(waiter());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  while (!m.try_lock()) {
    std::this_thread::yield();
  }
  m.unlock();

  source.request_cancellation();
  fut.get();

  EXPECT_TRUE(cancelled.load(std::memory_order_acquire));
  EXPECT_TRUE(lock_reacquired.load(std::memory_order_acquire));
}

TEST(ConditionVariableTest, NotifyAllResumesAllWaiters) {
  mutex m;
  condition_variable cv;

  bool ready = false;
  std::atomic<int> waiting_count{0};
  std::atomic<int> resumed_count{0};

  auto waiter = [&]() -> task<> {
    co_await m.lock();
    while (!ready) {
      waiting_count.fetch_add(1, std::memory_order_acq_rel);
      co_await cv.wait(m);
      waiting_count.fetch_sub(1, std::memory_order_acq_rel);
    }
    resumed_count.fetch_add(1, std::memory_order_acq_rel);
    m.unlock();
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

  while (!m.try_lock()) {
    std::this_thread::yield();
  }
  ready = true;
  m.unlock();

  for (int i = 0; i < 128 && resumed_count.load(std::memory_order_acquire) < 2; ++i) {
    cv.notify_all();
    std::this_thread::yield();
  }

  fut1.get();
  fut2.get();
  EXPECT_EQ(resumed_count.load(std::memory_order_acquire), 2);
}

