#pragma once
#include <cassert>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"

namespace xcoro {
template <std::ptrdiff_t LeastMaxValue = (std::numeric_limits<std::ptrdiff_t>::max)()>
class counting_semaphore {
  static_assert(LeastMaxValue >= 0, "LeastMaxValue must be non-negative");

 public:
  explicit counting_semaphore(std::ptrdiff_t initial_count = 0) noexcept
      : count_(sanitize_initial_count(initial_count)) {
    assert(initial_count >= 0 && initial_count <= max());
  }

  counting_semaphore(const counting_semaphore&) = delete;
  counting_semaphore& operator=(const counting_semaphore&) = delete;

  static constexpr std::ptrdiff_t max() noexcept {
    return LeastMaxValue;
  }

  bool try_acquire() noexcept {
    std::lock_guard<std::mutex> guard(mtx_);
    if (count_ > 0) {
      --count_;
      return true;
    }
    return false;
  }
  void release(std::ptrdiff_t update = 1) noexcept {
    if (update <= 0) {
      assert(update >= 0);
      return;
    }
    std::vector<std::coroutine_handle<>> to_resume;
    to_resume.reserve(static_cast<size_t>(update));
    {
      std::lock_guard<std::mutex> guard(mtx_);
      for (std::ptrdiff_t i = 0; i < update; ++i) {
        bool resumed = false;
        while (!waiters_.empty()) {
          auto waiter = waiters_.front();
          waiters_.pop();
          if (!waiter || !waiter->try_mark_resumed()) {
            continue;
          }
          if (waiter->should_resume_now()) {
            to_resume.push_back(waiter->handle);
          }
          resumed = true;
          break;
        }
        // 这次发出的许可没有交给任何等待者，所以存起来count_++
        if (!resumed) {
          if (count_ == max()) {
            assert(false && "counting_semaphore::release would exceed max()");
            continue;
          }
          ++count_;
        }
      }
    }
    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }
  auto acquire() noexcept { return awaiter{*this}; }
  auto acquire(cancellation_token token) noexcept {
    return awaiter{*this, std::move(token)};
  }

 private:
  static constexpr std::ptrdiff_t sanitize_initial_count(std::ptrdiff_t count) noexcept {
    if (count < 0) {
      return 0;
    }
    if (count > max()) {
      return max();
    }
    return count;
  }

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
  struct awaiter {
    awaiter(counting_semaphore& sem, cancellation_token token = {}) noexcept
        : sem_(sem), token_(std::move(token)) {}
    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return sem_.try_acquire();  // 先尝试避免挂起
    }
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;  // 不挂起
      }
      auto state = std::make_shared<waiter_state>();
      state->handle = handle;
      if (!sem_.enqueue_waiter(state)) {
        return false;
      }
      state_ = state;

      if (token_.can_be_cancelled()) {
        reg_ = cancellation_registration(token_, [state]() noexcept {
          if (!state->try_mark_resumed()) {
            return;
          }
          // 标记是因为cancellation而导致的协程恢复
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
      if (cancelled_immediate_ || state_ && state_->cancelled.load(std::memory_order_acquire)) {
        throw operation_cancelled{};  // 抛出异常
      }
    }

   private:
    counting_semaphore& sem_;
    cancellation_token token_;
    cancellation_registration reg_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mtx_);
    if (count_ > 0) {
      --count_;
      return false;
    }
    waiters_.push(state);
    return true;
  }
  std::mutex mtx_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

using semaphore = counting_semaphore<>;
using binary_semaphore = counting_semaphore<1>;

}  // namespace xcoro
