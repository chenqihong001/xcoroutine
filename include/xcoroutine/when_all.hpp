#pragma once
#include "awaitable_traits.hpp"
#include "concepts/awaitable.hpp"
#include "utils/void_value.hpp"
#include <algorithm>
#include <atomic>
#include <coroutine>
#include <exception>
#include <tuple>
#include <type_traits>
#include <variant>

namespace xcoro {

namespace detail {

class when_all_latch {
public:
  explicit when_all_latch(std::size_t count) noexcept
      : count_(count + 1) {} // +1是为了try_await中的初始减1

  bool is_ready() const noexcept {
    // 当计数为1时表示只剩try_await中的那一次了，所有任务都已完成
    return count_.load(std::memory_order_acquire) == 1;
  }

  bool try_await(std::coroutine_handle<> awaiting_coroutine) noexcept {
    awaiting_coroutine_ = awaiting_coroutine;
    return count_.fetch_sub(1, std::memory_order_acq_rel) > 1;
    // return true 表示还有任务未完成 父协程继续挂起
    // return false 表示所有任务已完成  返回执行父协程
  }

  // 这个函数的执行体是when_all_task
  void notify_awaitable_completed() noexcept {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // 所有任务执行结束
      awaiting_coroutine_.resume(); // 恢复等待/父协程
    }
  }

private:
  std::atomic<std::size_t> count_;
  std::coroutine_handle<> awaiting_coroutine_;
};

template <typename T> class when_all_task;

template <typename T> class when_all_ready_awaitable;

template <> class when_all_ready_awaitable<std::tuple<>> {
public:
  when_all_ready_awaitable() noexcept {}
  explicit when_all_ready_awaitable(std::tuple<>) noexcept {}
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) noexcept {}
  std::tuple<> await_resume() noexcept { return {}; }
};

template <typename... Task>
class when_all_ready_awaitable<std::tuple<Task...>> {
public:
  explicit when_all_ready_awaitable(Task &&...tasks) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<Task>...>)
      : latch_(sizeof...(Task)), tasks_(std::move(tasks)...) {}

  explicit when_all_ready_awaitable(std::tuple<Task...> &&tasks) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<Task>...>)
      : latch_(sizeof...(Task)), tasks_(std::move(tasks)) {}

  auto operator co_await() & noexcept {
    struct awaiter {
      awaiter(when_all_ready_awaitable &awaitbale) : awaitable_(awaitbale) {}
      bool await_ready() const noexcept { return awaitable_.is_ready(); }
      bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return awaitable_.try_await(handle);
        // return true挂起，return false 不挂起
      }
      auto await_resume() {
        // 检查并重新抛出异常
        std::apply(
            [](auto &...tasks) {
              (tasks.promise().rethrow_if_exception(), ...);
            },
            awaitable_.tasks_);

        // 返回所有任务的结果
        return std::apply(
            [](auto &...tasks) {
              return std::tuple<typename std::remove_reference_t<
                  decltype(tasks)>::promise_type::value_type &...>{
                  tasks.promise().result()...};
            },
            awaitable_.tasks_);
      }

    private:
      when_all_ready_awaitable &awaitable_;
    };
    return awaiter{*this};
  }

  auto operator co_await() && noexcept {
    struct awaiter {
      awaiter(when_all_ready_awaitable &awaitbale) : awaitable_(awaitbale) {}
      bool await_ready() const noexcept { return awaitable_.is_ready(); }
      bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return awaitable_.try_await(handle);
        // return true挂起，return false 不挂起
      }
      auto await_resume() {
        // 检查并重新抛出异常
        std::apply(
            [](auto &...tasks) {
              (tasks.promise().rethrow_if_exception(), ...);
            },
            awaitable_.tasks_);

        // 返回所有任务的结果（移动语义）
        return std::apply(
            [](auto &...tasks) {
              return std::tuple<typename std::remove_reference_t<
                  decltype(tasks)>::promise_type::value_type &&...>{
                  std::move(tasks.promise().result())...};
            },
            awaitable_.tasks_);
      }

    private:
      when_all_ready_awaitable &awaitable_;
    };
    return awaiter{*this};
  }

private:
  bool is_ready() const noexcept { return latch_.is_ready(); }
  bool try_await(std::coroutine_handle<> handle) noexcept {
    std::apply([this](auto &&...tasks) { ((tasks.start(latch_)), ...); },
               tasks_);
    return latch_.try_await(handle);
  }

  when_all_latch latch_;
  std::tuple<Task...> tasks_;
};

// R -> const T & , T , T&  void类型
template <typename R> class when_all_task_promise {
public:
  when_all_task_promise() noexcept {}

  when_all_task<R> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }

  // 处理make_when_all_task中co_return的情况
  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() const noexcept { return false; }
      void await_suspend(
          std::coroutine_handle<when_all_task_promise> handle) noexcept {
        handle.promise().latch_->notify_awaitable_completed();
      }
      void await_resume() noexcept {}
    };
    return completion_notifier{};
  }

  void unhandled_exception() noexcept { result_ = std::current_exception(); }

  // 只会传入右值版本
  auto yield_value(R &&result) noexcept {
    result_ = std::move(result);
    return final_suspend(); // 手动调用final_suspend()，处理latch的减少
  }

  // result() 函数，返回value_type引用
  // value_type &result() & noexcept { return *result_; }
  // value_type &&result() && noexcept { return std::move(*result_); }

  auto result() noexcept {
    if constexpr (std::holds_alternative<std::remove_cvref_t<R>>(result_)) {
      return std::move(std::get<std::remove_cvref_t<R>>(result_));
    } else {
      std::rethrow_exception(std::get<std::exception_ptr>(result_));
    }
  }

  // 检查并重新抛出异常

  void return_void() noexcept {}

  void start(when_all_latch &latch) noexcept {
    latch_ = &latch;
    std::coroutine_handle<when_all_task_promise>::from_promise(*this).resume();
  }

private:
  when_all_latch *latch_ = nullptr;
  std::variant<std::remove_cvref_t<R>, std::exception_ptr> result_;
};

// 特化T&
template <typename R> class when_all_task_promise<R &> {
public:
private:
  when_all_latch *latch_ = nullptr;
  std::variant<std::add_pointer_t<R>, std::exception_ptr> result_;
};

// 特化void
template <> class when_all_task_promise<void> {
public:
private:
  when_all_latch *latch_ = nullptr;
  std::exception_ptr exception_;
};

// R -> const T & , T , T&  void
template <typename R> class when_all_task {
public:
  using promise_type = when_all_task_promise<R>;

  when_all_task(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  when_all_task(when_all_task &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  when_all_task &operator=(when_all_task &&other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  // 禁止拷贝
  when_all_task(const when_all_task &) = delete;
  when_all_task &operator=(const when_all_task &) = delete;

  ~when_all_task() {
    if (handle_) {
      handle_.destroy();
    }
  }

private:
  void start(when_all_latch &latch) noexcept { handle_.promise().start(latch); }
  std::coroutine_handle<promise_type> handle_;
};

template <typename T>
when_all_task<T> when_all_task_promise<T>::get_return_object() noexcept {
  return when_all_task<T>{
      std::coroutine_handle<when_all_task_promise<T>>::from_promise(*this)};
}

template <concepts::Awaitable Awaitable,
          typename R = awaiter_result_t<Awaitable>>
when_all_task<R> make_when_all_task(Awaitable &&awaitable) {
  if constexpr (std::is_void_v<R>) {
    // std::forward
    // 可以保证必要时调用co_await&&版本，而不是全部调用co_await&版本
    co_await std::forward<Awaitable>(awaitable);
    co_return;
  } else {
    co_yield co_await std::forward<Awaitable>(awaitable);
  }
}

} // namespace detail

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_all(Awaitables &&...awaitables) {
  return detail::when_all_ready_awaitable<
      std::tuple<detail::when_all_task<awaiter_result_t<Awaitables>>>...>(
      std::make_tuple(detail::make_when_all_task(std::move(awaitables))...));
}

} // namespace xcoro

// when_all_ready_awaitable 通过 std::tuple<>构造

// detail::when_all_ready_awaitable<std::tuple<detail::when_all_task<typename
// concepts::awaitable_traits<awaitables_type>::awaiter_return_type>...>>(std::make_tuple(detail::make_when_all_task(std::move(awaitables))...))
