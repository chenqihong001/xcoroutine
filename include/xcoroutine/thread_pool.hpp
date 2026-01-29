#pragma once

#include <atomic>
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
      : thread_count_(thread_count), task_queues_(thread_count), stop_(false) {
    start();
  }

  ~thread_pool() { stop(); }

  // 2. 调度操作
  class schedule_operation {
  public:
    schedule_operation(thread_pool &pool) : pool_(pool) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
      pool_.schedule_impl(handle);
    }

    void await_resume() const noexcept {}

  private:
    thread_pool &pool_;
  };

  // 3. yield操作
  class yield_operation {
  public:
    yield_operation(thread_pool &pool) : pool_(pool) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
      pool_.schedule_impl(handle, true); // yield的重新调度
    }

    void await_resume() const noexcept {}

  private:
    thread_pool &pool_;
  };

  [[nodiscard]] schedule_operation schedule() {
    return schedule_operation(*this);
  }

  [[nodiscard]] yield_operation yield() noexcept {
    return yield_operation(*this);
  }

  // 4. 获取线程数
  size_t thread_count() const noexcept { return threads_.size(); }

  // 5. 停止线程池
  void stop() noexcept {
    if (stop_.exchange(true)) {
      return;
    }

    cv_.notify_all();

    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  // 6. 获取待处理任务数量
  size_t pending_tasks() const noexcept {
    size_t total = 0;
    for (const auto &queue : task_queues_) {
      total += queue.size();
    }
    return total;
  }

private:
  // 7. 启动线程池
  void start() {
    threads_.reserve(thread_count_);
    for (uint32_t i = 0; i < thread_count_; ++i) {
      threads_.emplace_back(&thread_pool::worker_thread, this, i);
    }
  }

  // 8. 工作窃取队列
  class work_stealing_queue {
  public:
    friend class thread_pool;
    work_stealing_queue() = default;

    // 插入队尾
    void push(std::coroutine_handle<> handle) {
      std::lock_guard lock(mutex_);
      queue_.push_back(handle);
    }

    // 本地线程从队尾弹出
    bool try_pop(std::coroutine_handle<> &handle) {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        return false;
      }
      handle = queue_.back();
      queue_.pop_back();
      return true;
    }

    // 其他线程从队头窃取
    bool try_steal(std::coroutine_handle<> &handle) {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        return false;
      }
      // 从队头窃取（FIFO窃取）
      handle = queue_.front();
      queue_.pop_front();
      return true;
    }

    bool empty() const {
      std::lock_guard lock(mutex_);
      return queue_.empty();
    }

    size_t size() const {
      std::lock_guard lock(mutex_);
      return queue_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::deque<std::coroutine_handle<>> queue_;
  };

  // 9. 线程局部存储
  struct thread_local_data {
    static inline thread_local int thread_index = -1;
    static inline thread_local std::mt19937 random_engine{
        std::random_device{}()};
  };

  // 10. 调度实现 - 核心函数
  void schedule_impl(std::coroutine_handle<> handle, bool is_yield = false) {
    if (!handle || handle.done()) {
      return;
    }

    // 获取当前线程索引
    int current_thread = thread_local_data::thread_index;

    // 如果在线程池线程中调用
    if (current_thread >= 0 &&
        current_thread < static_cast<int>(thread_count_)) {
      // yield操作：放到队列尾部，给其他任务机会
      if (is_yield) {
        task_queues_[current_thread].push(handle);
      } else {
        // 普通调度：直接放入当前线程队列
        task_queues_[current_thread].push(handle);
      }
    } else {
      // 从外部线程调用：使用负载均衡策略
      uint32_t target_index = get_least_loaded_queue();
      task_queues_[target_index].push(handle);
    }

    // 唤醒等待的线程
    cv_.notify_one();
  }

  // 11. 获取负载最小的队列索引
  uint32_t get_least_loaded_queue() const {
    uint32_t min_index = 0;
    size_t min_size = task_queues_[0].size();

    for (uint32_t i = 1; i < thread_count_; ++i) {
      size_t size = task_queues_[i].size();
      if (size < min_size) {
        min_size = size;
        min_index = i;
      }
    }

    return min_index;
  }

  // 12. 工作线程主循环
  void worker_thread(uint32_t thread_index) {
    // 设置线程局部存储
    thread_local_data::thread_index = thread_index;

    auto &my_queue = task_queues_[thread_index];

    while (!stop_.load(std::memory_order_relaxed)) {
      std::coroutine_handle<> task;

      // 阶段1：尝试从自己的队列获取任务（LIFO - 更好的缓存局部性）
      if (my_queue.try_pop(task)) {
        if (task && !task.done()) {
          task.resume();
        }
        continue;
      }

      // 阶段2：尝试从其他队列窃取任务（FIFO - 窃取老任务）
      if (try_steal_task(thread_index, task)) {
        if (task && !task.done()) {
          task.resume();
        }
        continue;
      }

      // 阶段3：等待新任务
      std::unique_lock lock(cv_mutex_);

      // 双重检查：避免在获取锁之前有新任务到来
      if (!my_queue.empty()) {
        continue;
      }

      if (stop_.load(std::memory_order_relaxed)) {
        break;
      }

      // 检查是否真的所有队列都为空（避免虚假唤醒）
      bool has_work = false;
      for (const auto &queue : task_queues_) {
        if (!queue.empty()) {
          has_work = true;
          break;
        }
      }

      if (!has_work) {
        // 等待一小段时间或被唤醒
        cv_.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
  }

  // 13. 窃取任务（随机化策略以减少竞争）
  bool try_steal_task(uint32_t thief_index, std::coroutine_handle<> &task) {
    // 随机选择起始受害者
    std::uniform_int_distribution<uint32_t> dist(0, thread_count_ - 1);
    uint32_t start_index = dist(thread_local_data::random_engine);

    // 遍历所有队列尝试窃取
    for (uint32_t i = 0; i < thread_count_; ++i) {
      uint32_t victim_index = (start_index + i) % thread_count_;

      // 跳过自己的队列
      if (victim_index == thief_index) {
        continue;
      }

      auto &victim_queue = task_queues_[victim_index];
      if (victim_queue.try_steal(task)) {
        return true;
      }
    }

    return false;
  }

private:
  uint32_t thread_count_;
  std::vector<work_stealing_queue> task_queues_; // 每个线程一个队列
  std::vector<std::thread> threads_;

  // 同步原语
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  std::atomic_bool stop_;

  // 禁用复制和移动
  thread_pool(const thread_pool &) = delete;
  thread_pool &operator=(const thread_pool &) = delete;
  thread_pool(thread_pool &&) = delete;
  thread_pool &operator=(thread_pool &&) = delete;
};

} // namespace xcoro