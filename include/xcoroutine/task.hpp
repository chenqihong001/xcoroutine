#pragma once
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <type_traits>
#include <variant>
namespace xcoro {
template <typename T> class task;

namespace detail {

class task_promise_base {
  friend class final_awaiter;
  struct final_awaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
      if (continue_handle_) {
        return continue_handle_;
      }
      return std::noop_coroutine(); // 没有继续的协程 直接返回空句柄，返回主程序
    }
    void await_resume() noexcept {} // 返回值被忽略
    std::coroutine_handle<> continue_handle_;
  };

public:
  task_promise_base() = default;
  ~task_promise_base() = default;

  std::suspend_always initial_suspend() noexcept { return {}; }
  final_awaiter final_suspend() noexcept { return {continuation_}; }

  void set_continuation(std::coroutine_handle<> continuation) noexcept {
    continuation_ = continuation;
  }

protected:
  std::coroutine_handle<> continuation_;
};

template <typename T> class task_promise final : public task_promise_base {
  task_promise(task_promise &&) = delete;
  task_promise(const task_promise &) = delete;
  task_promise &operator=(const task_promise &) = delete;
  task_promise &operator=(task_promise &&) = delete;

  using value_type =
      std::conditional_t<std::is_reference_v<T>, std::add_pointer_t<T>,
                         std::remove_const_t<T>>;
  // remove_const_t 把 tasl<const int> 转成 task<int>

public:
  task_promise() =
      default; // 编译器会自动调用promise_type的构造函数，创建promise对象
  ~task_promise() = default;
  task<T> get_return_object() noexcept;

  template <typename U>
    requires std::is_constructible_v<T, U &&>
  void return_value(U &&value) noexcept {
    if constexpr (std::is_reference_v<T>) {
      result_.template emplace<value_type>(std::addressof(value));
    } else {

      result_.template emplace<T>(std::forward<U>(value));
    }
  }
  void unhandled_exception() noexcept {
    result_.template emplace<std::exception_ptr>(std::current_exception());
  }

  // 用于场景 auto pro = promise{}; co_await pro;
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

private:
  std::variant<std::monostate, value_type, std::exception_ptr> result_;
};

// 特化void版本
template <> class task_promise<void> final : public task_promise_base {
public:
  task_promise() = default;
  task_promise(task_promise &&) = delete;
  task_promise(const task_promise &) = delete;
  task_promise &operator=(const task_promise &) = delete;
  task_promise &operator=(task_promise &&) = delete;

public:
  task<void> get_return_object() noexcept;
  void return_void() noexcept {}
  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  void result() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

private:
  std::exception_ptr exception_;
};

} // namespace detail

template <typename T = void> class [[nodiscard]] task {
public:
  using promise_type = detail::task_promise<T>;
  explicit task(std::coroutine_handle<detail::task_promise<T>> handle) {
    handle_ = handle;
  }
  task(const task &) = delete;
  task &operator=(const task &) = delete;

  task(task &&other) {
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  task &operator=(task &&other) {
    if (std::addressof(other) == this) {
      return *this;
    }
    if (handle_) {
      handle_.destroy();
    }
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
  }

  ~task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::coroutine_handle<detail::task_promise<T>> handle() const noexcept {
    return handle_;
  }

  auto operator co_await() const & noexcept {
    struct awaitable : public awaitbale_base {
      decltype(auto) await_resume() {
        return this->coroutine_.promise().result(); // 保留引用
      }
    };
    return awaitable{handle_};
  }

  auto operator co_await() const && noexcept {

    struct awaitable : public awaitbale_base {
      decltype(auto) await_resume() {
        return std::move(this->coroutine_.promise())
            .result(); // 调用右值版本的result函数
                       // 返回 T&， const T&&
      }
    };
    return awaitable{handle_};
  }

  struct awaitbale_base {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> continuation) noexcept {
      coroutine_.promise().set_continuation(continuation);
      return coroutine_;
      // 执行当前协程，保留父协程句柄
    }
    std::coroutine_handle<promise_type> coroutine_;
  };

private:
  std::coroutine_handle<detail::task_promise<T>> handle_;
};

template <typename T>
task<T> detail::task_promise<T>::get_return_object() noexcept {
  return task<T>(std::coroutine_handle<task_promise<T>>::from_promise(*this));
}

inline task<void> detail::task_promise<void>::get_return_object() noexcept {
  return task<void>(
      std::coroutine_handle<task_promise<void>>::from_promise(*this));
}

} // namespace xcoro