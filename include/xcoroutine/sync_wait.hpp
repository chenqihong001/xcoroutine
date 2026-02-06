#pragma once
#include "awaitable_traits.hpp"
#include "concepts/awaitable.hpp"
#include "utils/nocopyable.hpp"
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

namespace xcoro {

namespace detail {

class sync_wait_event : public nocopyable {
public:
  sync_wait_event(bool initially_set = false) : is_set_(initially_set) {}
  ~sync_wait_event() = default;

  void set() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    is_set_.exchange(true);
    cv_.notify_all();
  }

  void reset() noexcept { is_set_.exchange(false); }
  void wait() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]() { return is_set_.load(); });
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic_bool is_set_ = false;
};

template <typename T> class sync_wait_task;

template <typename T> class sync_wait_task_promise final : public nocopyable {
public:
  sync_wait_task_promise() = default;
  ~sync_wait_task_promise() = default;
  using coroutine_handle = std::coroutine_handle<sync_wait_task_promise>;
  using value_type =
      std::conditional_t<std::is_reference_v<T>, std::add_pointer_t<T>,
                         std::remove_const_t<T>>;

  sync_wait_task<T> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept {
    result_.template emplace<std::exception_ptr>(std::current_exception());
  }
  template <typename U>
    requires std::is_constructible_v<T, U &&>
  void return_value(U &&value) noexcept {
    if constexpr (std::is_reference_v<T>) {
      result_.template emplace<value_type>(std::addressof(value));
    } else {

      result_.template emplace<T>(std::forward<U>(value));
    }
  }

  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() noexcept { return false; }
      void await_suspend(coroutine_handle h) noexcept {
        h.promise().event_->set();
      }
      void await_resume() noexcept {}
    };
    return completion_notifier{};
  }

  T result() {
    if (std::holds_alternative<value_type>(result_)) {
      if constexpr (std::is_reference_v<T>) {
        // T 是引用类型，直接返回引用
        return static_cast<T>(*std::get<value_type>(result_));
      } else {
        // T 是值类型，移动返回
        return std::move(std::get<value_type>(result_));
      }
    } else if (std::holds_alternative<std::exception_ptr>(result_)) {
      std::rethrow_exception(std::get<std::exception_ptr>(result_));
    } else {
      throw std::runtime_error("Task result not ready");
    }
  }
  void set_event(sync_wait_event *event) noexcept { event_ = event; }

private:
  std::variant<std::monostate, value_type, std::exception_ptr> result_;
  sync_wait_event *event_ = nullptr;
};

template <> class sync_wait_task_promise<void> : public nocopyable {
public:
  sync_wait_task_promise() = default;
  ~sync_wait_task_promise() = default;
  using coroutine_handle = std::coroutine_handle<sync_wait_task_promise>;

  sync_wait_task<void> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  void return_void() noexcept {}

  void result() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() noexcept { return false; }
      void await_suspend(coroutine_handle h) noexcept {
        h.promise().event_->set();
      }
      void await_resume() noexcept {}
    };
    return completion_notifier{};
  }
  void set_event(sync_wait_event *event) noexcept { event_ = event; }

private:
  sync_wait_event *event_ = nullptr;
  std::exception_ptr exception_;
};

template <typename T> class sync_wait_task : public nocopyable {
public:
  using promise_type = sync_wait_task_promise<T>;
  explicit sync_wait_task(std::coroutine_handle<promise_type> handle)
      : handle_(handle) {}
  void start(sync_wait_event &event) {
    handle_.promise().set_event(
        &event); // 必须在resume之前设置event，否则在final_suspend中会访问空指针
    std::coroutine_handle<promise_type>::from_promise(handle_.promise())
        .resume();
  }
  promise_type &promise() & { return handle_.promise(); }
  promise_type &&promise() && { return std::move(handle_.promise()); }

private:
  std::coroutine_handle<promise_type> handle_;
};

template <typename T>
sync_wait_task<T> sync_wait_task_promise<T>::get_return_object() noexcept {
  return sync_wait_task<T>(
      std::coroutine_handle<sync_wait_task_promise<T>>::from_promise(*this));
}

inline sync_wait_task<void>
sync_wait_task_promise<void>::get_return_object() noexcept {
  return sync_wait_task<void>(
      std::coroutine_handle<sync_wait_task_promise<void>>::from_promise(*this));
}

template <concepts::Awaitable T, typename R = awaiter_result_t<T>>
static auto make_sync_wait_task(T &&awaitable) -> sync_wait_task<R> {
  if constexpr (std::is_void_v<R>) {
    co_await std::forward<T>(awaitable);
    co_return;
  } else {
    co_return co_await std::forward<T>(awaitable);
  }
}

} // namespace detail

template <concepts::Awaitable T, typename R = awaiter_result_t<T>>
decltype(auto) sync_wait(T &&awaitable) {
  // sync_wait的职责是运行make_sync_wait_task协程，去执行awaitable，并收集结果，在任务未完成时，阻塞
  detail::sync_wait_event event;
  auto task = detail::make_sync_wait_task(std::forward<T>(awaitable));
  task.start(event);
  event.wait();
  if constexpr (std::is_void_v<R>) {
    task.promise().result();
  } else if constexpr (std::is_rvalue_reference_v<R>) {
    return std::move(task.promise()).result();
  } else {
    return task.promise().result();
  }
}

} // namespace xcoro