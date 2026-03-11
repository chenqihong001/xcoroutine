#pragma once
#include <atomic>
#include <coroutine>

namespace xcoro {

/**
 * @brief 手动触发的事件同步原语
 * 允许多个协程等待同一个事件，当事件被触发时，所有等待协程会被恢复执行
 * @note 线程安全，可在多线程环境中使用
 */
class manual_reset_event {
 public:
  struct awaiter {
    awaiter(const manual_reset_event& e) : event_(e) {}

    bool await_ready() const noexcept { return event_.is_set(); }
    bool await_suspend(std::coroutine_handle<> awaiting_handle) noexcept {
      awaiting_coroutine_ = awaiting_handle;
      void* old_value = event_.state_.load(std::memory_order_acquire);
      do {
        // 如果事件已经触发，立即触发协程
        if (old_value == &event_) {
          return false;  // 不挂起
        }
        // 事件未触发，将当前等待的协程插入链表头部
        next_ = static_cast<awaiter*>(old_value);
        // 先把head_指向old_value
        // 更新head需要保证原子操作，操作失败，在循环的时候，会自动重新更新next_指向
      } while (!event_.state_.compare_exchange_weak(old_value, this,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
      return true;  // 成功挂起
    }
    void await_resume() noexcept {}
    awaiter* next_;                               // 下一个等待者（链表结构）
    std::coroutine_handle<> awaiting_coroutine_;  // 等待的协程句柄
    const manual_reset_event& event_;             // 关联的事件
  };

  explicit manual_reset_event() : state_(nullptr) {}

  // 检查事件是否已触发
  bool is_set() const noexcept { return state_.load(std::memory_order_acquire) == this; }

  // 触发事件，恢复所有等待者
  void set() noexcept {
    // 原子交换状态为“已触发”，state_ = this
    void* old_value = state_.exchange(this, std::memory_order_acq_rel);

    if (old_value != this && old_value != nullptr) {
      // 恢复所有等待者
      auto* waiters = static_cast<awaiter*>(old_value);
      while (waiters != nullptr) {
        auto* next = waiters->next_;
        waiters->awaiting_coroutine_.resume();
        waiters = next;
      }
    }
  }

  auto operator co_await() const noexcept {
    // static_assert(std::is_same_v<decltype(*this), const manual_reset_event &>);
    return awaiter{*this};
  }

 private:
  /**
   * @brief 状态指针
   * - nullptr: 未触发，无等待者
   * - awaiter*: 未触发，有等待者（指向等待链表的头）
   * - this: 已触发
   */
  mutable std::atomic<void*> state_;
  friend class awaiter;
};

using event = manual_reset_event;

}  // namespace xcoro
