#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"

namespace xcoro {

class latch {
 public:
  explicit latch(std::ptrdiff_t count) : count_(count >= 0 ? count : 0) {}

  latch(const latch&) = delete;
  latch& operator=(const latch&) = delete;

  void count_down(std::ptrdiff_t n = 1) noexcept {
    if (n <= 0) {
      return;
    }

    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (count_ == 0) {
        return;
      }

      if (n < count_) {
        count_ -= n;
        return;
      }

      count_ = 0;
      while (!waiters_.empty()) {
        auto waiter = waiters_.front();
        waiters_.pop();
        if (!waiter || !waiter->try_mark_resumed()) {
          continue;
        }
        if (waiter->should_resume_now()) {
          to_resume.push_back(waiter->handle);
        }
      }
    }

    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }

  bool try_wait() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return count_ == 0;
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
    awaiter(latch& l, cancellation_token token = {}) noexcept
        : latch_(l), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return latch_.try_wait();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      auto state = std::make_shared<waiter_state>();
      state->handle = handle;
      if (!latch_.enqueue_waiter(state)) {
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
    latch& latch_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait() noexcept { return awaiter{*this}; }
  auto wait(cancellation_token token) noexcept { return awaiter{*this, std::move(token)}; }

 private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == 0) {
      return false;
    }
    waiters_.push(state);
    return true;
  }

  mutable std::mutex mutex_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

}  // namespace xcoro

