#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/mutex.hpp"

namespace xcoro {

class condition_variable {
 public:
  condition_variable() = default;
  condition_variable(const condition_variable&) = delete;
  condition_variable& operator=(const condition_variable&) = delete;

 private:
  struct waiter_state {
    enum class suspend_state : std::uint8_t {
      waiting_await_suspend,
      suspended,
      wake_requested,
    };

    std::coroutine_handle<> handle{};
    mutex* associated_mutex{};
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
    awaiter(condition_variable& cv, mutex& m, cancellation_token token = {}) noexcept
        : cv_(cv), mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      auto state = std::make_shared<waiter_state>();
      state->handle = handle;
      state->associated_mutex = &mutex_;
      cv_.enqueue_waiter(state);
      mutex_.unlock();
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
              if (state->associated_mutex) {
                state->associated_mutex->lock_and_resume(state->handle);
              }
            });
      }

      if (state->try_mark_suspended()) {
        return true;
      }

      return mutex_.lock_or_enqueue(handle);
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

   private:
    condition_variable& cv_;
    mutex& mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait(mutex& m) noexcept { return awaiter{*this, m}; }
  auto wait(mutex& m, cancellation_token token) noexcept {
    return awaiter{*this, m, std::move(token)};
  }

  void notify_one() noexcept {
    std::shared_ptr<waiter_state> w;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          w.reset();
          continue;
        }
        break;
      }
    }

    if (w && w->associated_mutex && w->should_resume_now()) {
      w->associated_mutex->lock_and_resume(w->handle);
    }
  }

  void notify_all() noexcept {
    std::vector<std::shared_ptr<waiter_state>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          continue;
        }
        to_resume.push_back(std::move(w));
      }
    }

    for (auto& w : to_resume) {
      if (w && w->associated_mutex && w->should_resume_now()) {
        w->associated_mutex->lock_and_resume(w->handle);
      }
    }
  }

 private:
  void enqueue_waiter(const std::shared_ptr<waiter_state>& w) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    waiters_.push(w);
  }

  std::mutex mutex_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

}  // namespace xcoro
