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
  explicit thread_pool(uint32_t thread_count = std::thread::hardware_concurrency())
      : thread_count_(thread_count == 0 ? 1u : thread_count),
        local_queues_(thread_count_) {
    start();
  }

  ~thread_pool() {
    stop();
  }
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

  [[nodiscard]] size_t pending_tasks() const noexcept {
    std::lock_guard lock(state_mutex_);
    return pending_tasks_;
  }
  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;
  thread_pool(thread_pool&&) = delete;
  thread_pool& operator=(thread_pool&&) = delete;

 private:
  // 任务窃取队列
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
      queue_.pop_front();  // 从前面去任务
      return true;
    }

   private:
    mutable std::mutex mutex_;
    std::deque<std::coroutine_handle<>> queue_;
  };

  enum class enqueue_kind {
    schedule,
    yield
  };

  struct tls_state {
    static inline thread_local thread_pool* current_pool = nullptr;
    static inline thread_local int current_index = -1;
    static inline thread_local std::mt19937 random_engine{std::random_device{}()};
  };

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
        // 池内schedule：优先放回当前worker的本地队列，这样可以保留局部性，也减少全局队列竞争
        local_queues_[static_cast<size_t>(worker_index)].push_bottom(handle);
      } else {
        // 两种情况都走这里
        // 1. 外部线程 schedule
        // 2. 池内yield
        // yield 退回全局队列而不是本地队列，能减低刚yield又被本线程拿回来
        global_queue_.push_back(handle);
      }
      ++pending_tasks_;
    }
    cv_.notify_one();
    return true;
  }

  [[nodiscard]] bool should_exit_locked() const noexcept {
    // 只有当：
    // 1. 已经stop，不再接收新任务
    // 2. 没有排队中的任务
    // 3. 没有正在resume的worker
    // 才允许线程退出
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
        // 只要队列里还有任务，或者已经满足退出条件，就立即醒来
        cv_.wait(lock, [this] {
          return pending_tasks_ != 0 || should_exit_locked();
        });
        if (should_exit_locked()) {
          break;  // 退出
        }

        // 尝试获取任务
        const bool got_task = try_take_task_locked(thread_index, task);
        if (!got_task) {
          // 理论上 pending_tasks_ > 0 时，这里不应该失败。
          // 保留这层保护是为了让实现更稳健一点，而不是赌状态永远完美。
          continue;
        }

        --pending_tasks_;
        ++active_workers_;
      }
      // 执行协程时，不持有state_mutex_，否则所有worker都会被串行化
      task.resume();
      bool need_wake_all = false;
      // 判断是否需要结束线程池
      {
        std::lock_guard lock(state_mutex_);
        --active_workers_;
        // stop() 之后，如果这个任务恰好是最后一个活跃任务
        // 需要把其他wait中的worker全部叫醒，让它们退出
        need_wake_all = should_exit_locked();
      }
      if (need_wake_all) {
        cv_.notify_all();
      }
    }
    tls_state::current_index = -1;
    tls_state::current_pool = nullptr;
  }
  bool try_take_task_locked(uint32_t thread_index, std::coroutine_handle<>& task) noexcept {
    // 取任务顺序：
    //
    // 1. 先取自己的本地队列
    // 2. 本地没有，再看全局队列
    // 3. 还没有，再尝试从别的 worker 那里偷

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
        // 如果偷到自己了，直接下一个
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
  // state_mutex_ 只负责“线程池生命周期状态”
  // - accepting_
  // - pending_tasks_
  // - active_workers_
  // - global_queue_
  // - cv_ 等待条件
  mutable std::mutex state_mutex_;
  std::condition_variable cv_;
  std::deque<std::coroutine_handle<>> global_queue_;  // 全局队列
  std::vector<work_stealing_queue> local_queues_;
  std::vector<std::thread> threads_;
  // true 表示还在正常接收任务
  // false 表示stop()已开始，不再接收新的schedule/yield
  bool accepting_{true};

  // 当前排队中但还没有被某个worker拿走的任务数
  // 它由state_mutex_保护，并且是cv_唤醒条件的一部分
  size_t pending_tasks_{0};

  // 当前正在执行task.resume()的worker数量
  // stop()后只有同时满足pending_task_ == 0 和active_workers_=0才说明整个池子真的排空了
  size_t active_workers_{0};
};

}  // namespace xcoro
