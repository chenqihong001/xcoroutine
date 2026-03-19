#include "xcoro/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "xcoro/manual_reset_event.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

using namespace xcoro;

TEST(ThreadPoolTest, StartsRequestedNumberOfWorkers) {
  thread_pool pool(2);
  EXPECT_EQ(pool.thread_count(), 2u);
}

TEST(ThreadPoolTest, ScheduleResumesCoroutineOnWorkerThread) {
  thread_pool pool(2);
  const std::thread::id caller_thread = std::this_thread::get_id();

  const std::thread::id resumed_thread =
      sync_wait([&]() -> task<std::thread::id> {
        EXPECT_FALSE(pool.running_in_this_pool());
        co_await pool.schedule();
        EXPECT_TRUE(pool.running_in_this_pool());
        co_return std::this_thread::get_id();
      }());

  EXPECT_NE(resumed_thread, caller_thread);
}

TEST(ThreadPoolTest, YieldLetsOtherReadyTaskRunFirst) {
  thread_pool pool(1);
  manual_reset_event first_started;
  std::vector<int> order;

  auto first = [&]() -> task<> {
    co_await pool.schedule();
    order.push_back(1);
    first_started.set();
    co_await pool.yield();
    order.push_back(3);
  };

  auto second = [&]() -> task<> {
    co_await first_started;
    co_await pool.schedule();
    order.push_back(2);
  };

  sync_wait(when_all(first(), second()));

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(ThreadPoolTest, ScheduleAfterStopFallsBackToInlineExecution) {
  thread_pool pool(1);
  pool.stop();

  const std::thread::id caller_thread = std::this_thread::get_id();
  const std::thread::id resumed_thread =
      sync_wait([&]() -> task<std::thread::id> {
        co_await pool.schedule();
        EXPECT_FALSE(pool.running_in_this_pool());
        co_return std::this_thread::get_id();
      }());

  EXPECT_EQ(resumed_thread, caller_thread);
}

TEST(ThreadPoolTest, IdleWorkerCanStealTasksFromBusyWorker) {
  thread_pool pool(2);
  manual_reset_event gate;
  std::mutex ids_mutex;
  std::set<std::thread::id> waiter_threads;
  std::thread::id trigger_thread;

  auto waiter = [&]() -> task<> {
    co_await gate;
    co_await pool.schedule();
    {
      std::lock_guard lock(ids_mutex);
      waiter_threads.insert(std::this_thread::get_id());
    }
  };

  auto trigger = [&]() -> task<> {
    co_await pool.schedule();
    trigger_thread = std::this_thread::get_id();

    // gate.set() 会在当前 worker 上 inline 恢复所有 waiter；
    // 它们随后再次 schedule，会把自己压进当前 worker 的本地队列。
    gate.set();

    // 让 owner worker 暂时忙住，给另一个空闲 worker 充分时间去偷任务。
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };

  sync_wait(when_all(trigger(), waiter(), waiter(), waiter(), waiter(),
                     waiter(), waiter(), waiter(), waiter()));

  bool saw_stolen_task = false;
  {
    std::lock_guard lock(ids_mutex);
    for (const auto& waiter_thread : waiter_threads) {
      if (waiter_thread != trigger_thread) {
        saw_stolen_task = true;
        break;
      }
    }
  }

  EXPECT_TRUE(saw_stolen_task);
}
