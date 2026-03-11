#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"

namespace xcoro {

class condition_variable;

class mutex {
 public:
  mutex() = default;
  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;

  bool try_lock() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return true;
    }
    return false;
  }

  void unlock() noexcept {
    std::coroutine_handle<> next;
    bool handed_off = false;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto waiter = waiters_.front();
        waiters_.pop();
        if (!waiter || !waiter->try_mark_resumed()) {
          continue;
        }
        handed_off = true;
        if (waiter->should_resume_now()) {
          next = waiter->handle;
        }
        break;
      }

      if (!handed_off) {
        locked_ = false;
        return;
      }
    }

    if (next) {
      next.resume();
    }
  }

 private:
  struct waiter_state {
    enum class suspend_state : std::uint8_t {
      waiting_await_suspend,
      suspended,
      wake_requested,
    };

    std::coroutine_handle<> handle{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};
    std::atomic<suspend_state> suspend_phase{suspend_state::waiting_await_suspend};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }

    bool try_mark_suspended() noexcept {
      auto expected = suspend_state::waiting_await_suspend;
      return suspend_phase.compare_exchange_strong(expected, suspend_state::suspended,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire);
    }

    bool should_resume_now() noexcept {
      auto expected = suspend_state::waiting_await_suspend;
      if (suspend_phase.compare_exchange_strong(expected, suspend_state::wake_requested,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return false;
      }
      return expected == suspend_state::suspended;
    }
  };

 public:
  struct awaiter {
    awaiter(mutex& m, cancellation_token token = {}) noexcept
        : mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return mutex_.try_lock();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      auto state = std::make_shared<waiter_state>();
      state->handle = handle;
      if (!mutex_.enqueue_waiter(state)) {
        return false;
      }
      state_ = state;

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (!state->should_resume_now()) {
                return;
              }
              if (state->handle) {
                state->handle.resume();
              }
            });
      }

      return state->try_mark_suspended();
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

   private:
    mutex& mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto lock() noexcept { return awaiter{*this}; }
  auto lock(cancellation_token token) noexcept { return awaiter{*this, std::move(token)}; }

 private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return false;
    }
    waiters_.push(state);
    return true;
  }

  bool lock_or_enqueue(std::coroutine_handle<> handle) noexcept {
    auto state = std::make_shared<waiter_state>();
    state->handle = handle;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!locked_) {
        locked_ = true;
        return false;
      }
      waiters_.push(state);
    }
    return state->try_mark_suspended();
  }

  void lock_and_resume(std::coroutine_handle<> handle) noexcept {
    if (lock_or_enqueue(handle)) {
      return;
    }
    if (handle) {
      handle.resume();
    }
  }

  std::mutex mutex_;
  bool locked_{false};
  std::queue<std::shared_ptr<waiter_state>> waiters_;

  friend class condition_variable;
};

}  // namespace xcoro

