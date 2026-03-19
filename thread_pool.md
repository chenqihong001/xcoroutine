# thread_pool 重写稿

你现在 [thread_pool.hpp](/home/chen/projects/xcoro/include/xcoro/thread_pool.hpp) 里的版本已经有了“每线程队列 + 窃取”的雏形，但它还差几块关键语义没有收稳：

- `stop()` 现在更像“立刻停机”，不是“停止接单后排空队列”
- `yield()` 仍然可能被同一个 worker 立刻取回，实际没有明显让出执行权
- `cv.wait_for(1ms)` 是轮询式等待，空闲时会空转
- `cv` 的等待条件和队列状态不是由同一把锁统一管理，后面继续做优化时很容易踩到丢唤醒/退出竞态

你刚刚说想“加上窃取队列算法”，那这次我就不再回避 `work stealing`，直接给你一版“语义先写稳，再保留窃取”的重写稿。

## 设计目标

这版重写同时满足下面几件事：

- 保留 `work stealing`
- 保留 `schedule()` / `yield()` 这两个 awaitable 接口
- `stop()` 不丢已入队任务
- 停止阶段新的 `schedule()` / `yield()` 不会把协程挂死
- 不用 `wait_for(1ms)` 轮询
- 明确区分“本地队列”、“外部注入队列”、“被窃取的一端”

## 队列模型

这里采用一个很常见、也比较适合协程线程池的模型：

### 1. 每个 worker 一个本地双端队列

- worker 自己 `push_back()`
- worker 自己 `pop_back()`
- 其他 worker 只能 `pop_front()` 去偷

这样做的直觉是：

- 本地继续执行更偏 LIFO，缓存局部性更好
- 被窃取时从旧任务那一端偷，减少和 owner 的正面竞争

### 2. 再加一个全局注入队列

外部线程提交任务时，不直接碰某个 worker 的本地队列，而是先进全局队列：

- 外部线程 `schedule()` -> 进入 `global_queue_`
- worker 先尝试跑自己本地队列
- 本地没活时，再看全局队列
- 还没有，再去偷别人的本地队列

这样做有两个好处：

- 外部线程不需要先做一次“选哪个 worker”的负载估计
- `yield()` 可以安全地退回全局队列，避免同一个 worker 立刻把自己又拿回来

### 3. `yield()` 不直接回本地队列

这是这次重写里很关键的一点。

如果本地队列策略是：

- `push_back()`
- `pop_back()`

那么当前协程 `co_await pool.yield()` 后如果还是 `push_back()` 回自己本地队列，当前 worker 下一轮很可能马上又 `pop_back()` 把它取回来，这个 `yield()` 就失真了。

所以这里改成：

- `schedule()` 在池内调用时，优先回当前 worker 的本地队列
- `yield()` 在池内调用时，退回 `global_queue_`

这不是绝对公平，但比“刚让出又立刻拿回自己”更接近大家对 `yield()` 的直觉。

## 参考实现

下面这版实现保留了 work-stealing，同时把停止语义和等待条件整理干净了。  
这份代码不是追求极限吞吐的最终版，而是“先正确、可读、可迁回头文件”的版本。

```cpp
#pragma once

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace xcoro {

class thread_pool {
 public:
  explicit thread_pool(
      uint32_t thread_count = std::thread::hardware_concurrency())
      : thread_count_(normalize_thread_count(thread_count)),
        local_queues_(thread_count_) {
    start();
  }

  ~thread_pool() { stop(); }

  class schedule_operation {
   public:
    explicit schedule_operation(thread_pool& pool) noexcept : pool_(pool) {}

    bool await_ready() const noexcept { return false; }

    // 返回 true:
    //   当前协程已经真正挂起，并被成功放入线程池，后续会由某个 worker 恢复。
    //
    // 返回 false:
    //   线程池已经停止接收新任务，此时不要真的挂起当前协程，
    //   而是让它继续 inline 往下执行，避免“挂起后没人恢复”。
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      return pool_.enqueue(handle, enqueue_kind::schedule);
    }

    void await_resume() const noexcept {}

   private:
    thread_pool& pool_;
  };

  class yield_operation {
   public:
    explicit yield_operation(thread_pool& pool) noexcept : pool_(pool) {}

    bool await_ready() const noexcept { return false; }

    // yield 的目标不是“换线程”，而是“把当前协程重新排队，让别的工作先跑”。
    //
    // 在这版 work-stealing 设计里，yield 不回本地队列，而是退回全局队列：
    // 这样可以减少当前 worker 立刻又把自己 pop 回来的概率。
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      return pool_.enqueue(handle, enqueue_kind::yield);
    }

    void await_resume() const noexcept {}

   private:
    thread_pool& pool_;
  };

  [[nodiscard]] schedule_operation schedule() noexcept {
    return schedule_operation(*this);
  }

  [[nodiscard]] yield_operation yield() noexcept {
    return yield_operation(*this);
  }

  [[nodiscard]] size_t thread_count() const noexcept {
    return threads_.size();
  }

  [[nodiscard]] bool running_in_this_pool() const noexcept {
    return tls_state::current_pool == this;
  }

  void stop() noexcept {
    {
      std::lock_guard lock(state_mutex_);
      if (!accepting_) {
        return;
      }

      // 从这一刻起不再接收新的 schedule / yield。
      // 但已经入队的任务仍然允许自然排空。
      accepting_ = false;
    }

    cv_.notify_all();

    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  [[nodiscard]] size_t pending_tasks() const noexcept {
    std::lock_guard lock(state_mutex_);
    return pending_tasks_;
  }

  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;
  thread_pool(thread_pool&&) = delete;
  thread_pool& operator=(thread_pool&&) = delete;

 private:
  class work_stealing_queue {
   public:
    void push_bottom(std::coroutine_handle<> handle) {
      std::lock_guard lock(mutex_);
      queue_.push_back(handle);
    }

    bool try_pop_bottom(std::coroutine_handle<>& handle) {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        return false;
      }

      handle = queue_.back();
      queue_.pop_back();
      return true;
    }

    bool try_steal_top(std::coroutine_handle<>& handle) {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        return false;
      }

      handle = queue_.front();
      queue_.pop_front();
      return true;
    }

   private:
    mutable std::mutex mutex_;
    std::deque<std::coroutine_handle<>> queue_;
  };

  enum class enqueue_kind {
    schedule,
    yield,
  };

  struct tls_state {
    static inline thread_local thread_pool* current_pool = nullptr;
    static inline thread_local int current_index = -1;
    static inline thread_local std::mt19937 random_engine{
        std::random_device{}()};
  };

  static uint32_t normalize_thread_count(uint32_t requested) noexcept {
    // hardware_concurrency() 可能返回 0，手动兜底成 1。
    return requested == 0 ? 1u : requested;
  }

  [[nodiscard]] int current_worker_index() const noexcept {
    if (tls_state::current_pool != this) {
      return -1;
    }
    return tls_state::current_index;
  }

  bool enqueue(std::coroutine_handle<> handle, enqueue_kind kind) noexcept {
    if (!handle || handle.done()) {
      return false;
    }

    {
      std::lock_guard lock(state_mutex_);

      // 停止阶段不再接收新任务。
      // 这里返回 false，不是“报错”，而是配合 await_suspend(false)
      // 让当前协程继续 inline 执行，避免挂死。
      if (!accepting_) {
        return false;
      }

      const int worker_index = current_worker_index();

      if (worker_index >= 0 && kind == enqueue_kind::schedule) {
        // 池内 schedule：优先放回当前 worker 的本地队列。
        // 这样可以保留局部性，也减少全局队列竞争。
        local_queues_[static_cast<size_t>(worker_index)].push_bottom(handle);
      } else {
        // 两种情况都走这里：
        // 1. 外部线程 schedule
        // 2. 池内 yield
        //
        // yield 退回全局队列而不是本地队列，能降低“刚 yield 又被自己拿回来”的概率。
        global_queue_.push_back(handle);
      }

      ++pending_tasks_;
    }

    cv_.notify_one();
    return true;
  }

  [[nodiscard]] bool should_exit_locked() const noexcept {
    // 只有当：
    // 1. 已经 stop，不再接收新任务
    // 2. 没有排队中的任务
    // 3. 没有正在 resume 的 worker
    // 才允许线程退出。
    return !accepting_ && pending_tasks_ == 0 && active_workers_ == 0;
  }

  void start() {
    threads_.reserve(thread_count_);
    for (uint32_t i = 0; i < thread_count_; ++i) {
      threads_.emplace_back(&thread_pool::worker_thread, this, i);
    }
  }

  void worker_thread(uint32_t thread_index) noexcept {
    tls_state::current_pool = this;
    tls_state::current_index = static_cast<int>(thread_index);

    while (true) {
      std::coroutine_handle<> task;

      {
        std::unique_lock lock(state_mutex_);

        // 这里不用 wait_for(1ms) 轮询。
        // 只要队列里还有任务，或者已经满足退出条件，就立即醒来。
        cv_.wait(lock, [this] {
          return pending_tasks_ != 0 || should_exit_locked();
        });

        if (should_exit_locked()) {
          break;
        }

        const bool got_task = try_take_task_locked(thread_index, task);
        if (!got_task) {
          // 理论上 pending_tasks_ > 0 时，这里不应该失败。
          // 保留这层保护是为了让实现更稳健一点，而不是赌状态永远完美。
          continue;
        }

        --pending_tasks_;
        ++active_workers_;
      }

      // 执行协程时不持有 state_mutex_，否则所有 worker 都会被串行化。
      task.resume();

      bool need_wake_all = false;
      {
        std::lock_guard lock(state_mutex_);
        --active_workers_;

        // stop() 之后，如果这个任务恰好是最后一个活跃任务，
        // 需要把其他 wait 中的 worker 全部叫醒，让它们退出。
        need_wake_all = should_exit_locked();
      }

      if (need_wake_all) {
        cv_.notify_all();
      }
    }

    tls_state::current_index = -1;
    tls_state::current_pool = nullptr;
  }

  bool try_take_task_locked(uint32_t thread_index,
                            std::coroutine_handle<>& task) noexcept {
    // 取任务顺序：
    //
    // 1. 先取自己的本地队列
    // 2. 本地没有，再看全局注入队列
    // 3. 还没有，再尝试从别的 worker 那里偷
    //
    // 这个顺序兼顾了局部性、外部提交延迟和负载均衡。
    if (local_queues_[thread_index].try_pop_bottom(task)) {
      return true;
    }

    if (!global_queue_.empty()) {
      task = global_queue_.front();
      global_queue_.pop_front();
      return true;
    }

    return try_steal_locked(thread_index, task);
  }

  bool try_steal_locked(uint32_t thief_index,
                        std::coroutine_handle<>& task) noexcept {
    if (thread_count_ <= 1) {
      return false;
    }

    // 随机选择起始 victim，避免所有空闲线程总从同一个位置开始扫描。
    std::uniform_int_distribution<uint32_t> dist(0, thread_count_ - 1);
    const uint32_t start = dist(tls_state::random_engine);

    for (uint32_t i = 0; i < thread_count_; ++i) {
      const uint32_t victim = (start + i) % thread_count_;
      if (victim == thief_index) {
        continue;
      }

      if (local_queues_[victim].try_steal_top(task)) {
        return true;
      }
    }

    return false;
  }

 private:
  const uint32_t thread_count_;

  // state_mutex_ 只负责“线程池生命周期状态”：
  // - accepting_
  // - pending_tasks_
  // - active_workers_
  // - global_queue_
  // - cv_ 的等待条件
  //
  // 本地双端队列自己的结构仍然由各自 queue mutex 保护。
  mutable std::mutex state_mutex_;
  std::condition_variable cv_;

  std::deque<std::coroutine_handle<>> global_queue_;
  std::vector<work_stealing_queue> local_queues_;
  std::vector<std::thread> threads_;

  // true  表示还在正常接收任务
  // false 表示 stop() 已开始，不再接收新的 schedule / yield
  bool accepting_{true};

  // 当前“排队中但还没被某个 worker 拿走”的任务数。
  // 它由 state_mutex_ 保护，并且是 cv_ 唤醒条件的一部分。
  size_t pending_tasks_{0};

  // 当前正在执行 task.resume() 的 worker 数量。
  // stop() 后只有同时满足 pending_tasks_ == 0 和 active_workers_ == 0
  // 才说明整个池子真的排空了。
  size_t active_workers_{0};
};

}  // namespace xcoro
```

## 这一版的关键点

### 1. 为什么还要保留一个 `global_queue_`

很多人一开始写 work-stealing 会只保留“每线程一个本地队列”，然后外部线程提交任务时再选一个“最空”的队列塞进去。

这种写法不是不能做，但会多出几个问题：

- 外部线程每次提交都要估算负载
- `yield()` 没地方优雅地回退
- 等待条件更难写得统一

加一个 `global_queue_` 后，路径会清晰很多：

- 外部提交先进入全局队列
- worker 空下来就能统一接活
- `yield()` 也有了一个自然的降优先级落点

### 2. 为什么 `yield()` 不进本地队列

因为本地队列是：

- owner `push_bottom()`
- owner `pop_bottom()`
- thief `steal_top()`

如果 `yield()` 还是 `push_bottom()`，那么当前 worker 很可能下一轮立刻又 `pop_bottom()` 到自己。  
这对 `yield()` 的直觉语义是不友好的。

这版把 `yield()` 放到 `global_queue_`，本质上是在做一件事：

- 刻意让当前协程先离开 owner 的“最近可执行位置”

这样别的任务更容易插到它前面。

### 3. 为什么 `pending_tasks_` 要放到 `state_mutex_` 下统一管理

这是为了解决等待和退出的正确性问题。

如果你把：

- 队列状态
- 停止状态
- 活跃 worker 数
- `cv` 等待条件

拆散到多把互不相关的锁甚至只靠原子变量，后面很容易出现：

- 某个 worker 看到“暂时没活”准备睡
- 另一个线程刚好把任务塞进去了并发了 notify
- 但因为等待条件和通知不是围绕同一组受保护状态组织的，那个 notify 被错过

这类问题非常烦，尤其协程线程池又比普通函数任务池更难排。

这版的做法是：

- `pending_tasks_`
- `active_workers_`
- `accepting_`
- `global_queue_`
- `cv_` 的 predicate

全部围绕 `state_mutex_` 来写。  
这样虽然没有做到最极致的无锁化，但语义明显稳得多。

### 4. `stop()` 的语义是什么

这里明确采用“停止接单，但排空已接任务”的策略：

- `accepting_ = false`
- 之后新的 `schedule()` / `yield()` 不再入队
- 已经在队列里的任务照常执行
- 已经在跑的协程如果后续又调用 `schedule()`，会退化成 inline 继续执行
- 等 `pending_tasks_ == 0 && active_workers_ == 0`，worker 才退出

这是协程场景里最不容易把逻辑挂死的 stop 语义。

## 和你当前版本相比，最重要的变化

- 保留了本地双端队列 + 随机起点窃取
- 增加了 `global_queue_`，专门接外部提交和 `yield()`
- 移除了 `wait_for(1ms)` 的空转等待
- 把退出条件从“只看 stop_”改成“停止接单 + 队列排空 + 没有活跃 worker”
- `await_suspend()` 改成返回 `bool`，停止阶段不再把协程挂死

## 还可以继续优化的地方

这版已经比你当前实现稳很多了，但如果后面你想继续打磨吞吐，还可以往下做：

- 让 task 获取路径减少对 `state_mutex_` 的持有时间
- 用更轻量的 parked-worker / semaphore 机制替代当前的 `cv`
- 让 `yield()` 根据本地队列长度选择“本地前端”还是“全局队列”
- 给外部提交增加批量注入路径

不过这些都建议建立在“先把这版语义测稳”的前提下。

## 建议补的测试

如果你后面要把这份实现同步回头文件，至少补这几类测试：

- `schedule()` 从外部线程进入线程池，协程能被 worker 恢复
- 池内 `schedule()` 更偏向回到当前 worker 本地队列
- `yield()` 后其他已排队任务有机会先执行
- `stop()` 之后不再接收新任务，但已入队任务能排空
- 单线程池下 `yield()` 不会死锁
- 多线程池下空闲 worker 能从忙线程偷到任务

如果你愿意，我下一步可以直接把这版 work-stealing 实现同步到 [thread_pool.hpp](/home/chen/projects/xcoro/include/xcoro/thread_pool.hpp)，再顺手把测试补上。  
