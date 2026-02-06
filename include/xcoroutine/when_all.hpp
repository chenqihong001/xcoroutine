#pragma once
#include "awaitable_traits.hpp"
#include "concepts/awaitable.hpp"
#include "utils/void_value.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
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

  void set_awaiting_coroutine(
      std::coroutine_handle<> awaiting_coroutine) noexcept {
    awaiting_coroutine_ = awaiting_coroutine;
  }

  bool try_await() noexcept {
    return count_.fetch_sub(1, std::memory_order_acq_rel) > 1;
    // return true 表示还有任务未完成 父协程继续挂起
    // return false 表示所有任务已完成  返回执行父协程
  }

  // 这个函数的执行体是when_all_task
  void notify_awaitable_completed() noexcept {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // 所有任务执行结束
      if (awaiting_coroutine_) {
        awaiting_coroutine_.resume(); // 恢复等待/父协程
      }
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
      : latch_(sizeof...(Task)), tasks_(std::forward<Task>(tasks)...) {}

  explicit when_all_ready_awaitable(std::tuple<Task...> &&tasks) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<Task>...>)
      : latch_(sizeof...(Task)), tasks_(std::move(tasks)) {}

  auto operator co_await() & noexcept {
    struct awaiter {
      explicit awaiter(when_all_ready_awaitable &awaitable)
          : awaitable_(awaitable) {}
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
      explicit awaiter(when_all_ready_awaitable &awaitable)
          : awaitable_(awaitable) {}
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
                  decltype(tasks)>::promise_type::value_type...>{
                  std::move(tasks).promise().result()...};
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
    latch_.set_awaiting_coroutine(handle);
    std::apply([this](auto &...tasks) { ((tasks.start(latch_)), ...); },
               tasks_);
    return latch_.try_await();
  }

  when_all_latch latch_;
  std::tuple<Task...> tasks_;
};

// R -> const T & , T , T&  void类型
template <typename R> class when_all_task_promise {
public:
  using value_type = std::conditional_t<std::is_reference_v<R>, R,
                                        std::remove_const_t<R>>;
  using storage_type =
      std::conditional_t<std::is_reference_v<R>,
                         std::add_pointer_t<std::remove_reference_t<R>>,
                         std::remove_const_t<R>>;

  when_all_task_promise() noexcept = default;

  when_all_task<R> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }

  // 处理make_when_all_task中co_return的情况
  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() const noexcept { return false; }
      void await_suspend(
          std::coroutine_handle<when_all_task_promise> handle) noexcept {
        if (handle.promise().latch_) {
          handle.promise().latch_->notify_awaitable_completed();
        }
      }
      void await_resume() noexcept {}
    };
    return completion_notifier{};
  }

  void unhandled_exception() noexcept {
    result_.template emplace<std::exception_ptr>(std::current_exception());
  }

  template <typename U>
    requires std::is_constructible_v<R, U &&>
  void return_value(U &&value) noexcept {
    if constexpr (std::is_reference_v<R>) {
      result_.template emplace<storage_type>(std::addressof(value));
    } else {
      result_.template emplace<storage_type>(std::forward<U>(value));
    }
  }

  void rethrow_if_exception() {
    if (std::holds_alternative<std::exception_ptr>(result_)) {
      std::rethrow_exception(std::get<std::exception_ptr>(result_));
    }
  }

  decltype(auto) result() & {
    rethrow_if_exception();
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*std::get<storage_type>(result_));
    } else {
      return std::get<storage_type>(result_);
    }
  }

  decltype(auto) result() && {
    rethrow_if_exception();
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*std::get<storage_type>(result_));
    } else {
      return std::move(std::get<storage_type>(result_));
    }
  }

  void start(when_all_latch &latch) noexcept {
    latch_ = &latch;
    std::coroutine_handle<when_all_task_promise>::from_promise(*this).resume();
  }

private:
  when_all_latch *latch_ = nullptr;
  std::variant<std::monostate, storage_type, std::exception_ptr> result_;
};

// 特化void
template <> class when_all_task_promise<void> {
public:
  using value_type = void_value;

  when_all_task_promise() noexcept = default;
  when_all_task<void> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() const noexcept { return false; }
      void await_suspend(
          std::coroutine_handle<when_all_task_promise> handle) noexcept {
        if (handle.promise().latch_) {
          handle.promise().latch_->notify_awaitable_completed();
        }
      }
      void await_resume() noexcept {}
    };
    return completion_notifier{};
  }

  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  void return_void() noexcept {}

  void rethrow_if_exception() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

  value_type &result() & {
    rethrow_if_exception();
    return value_;
  }

  value_type &&result() && {
    rethrow_if_exception();
    return std::move(value_);
  }

  void start(when_all_latch &latch) noexcept {
    latch_ = &latch;
    std::coroutine_handle<when_all_task_promise>::from_promise(*this).resume();
  }

private:
  when_all_latch *latch_ = nullptr;
  std::exception_ptr exception_;
  value_type value_{};
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

  void start(when_all_latch &latch) noexcept { handle_.promise().start(latch); }
  promise_type &promise() & noexcept { return handle_.promise(); }
  promise_type &&promise() && noexcept { return std::move(handle_.promise()); }

private:
  std::coroutine_handle<promise_type> handle_;
};

template <typename T>
when_all_task<T> when_all_task_promise<T>::get_return_object() noexcept {
  return when_all_task<T>{
      std::coroutine_handle<when_all_task_promise<T>>::from_promise(*this)};
}

inline when_all_task<void>
when_all_task_promise<void>::get_return_object() noexcept {
  return when_all_task<void>{
      std::coroutine_handle<when_all_task_promise<void>>::from_promise(*this)};
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
    co_return co_await std::forward<Awaitable>(awaitable);
  }
}

} // namespace detail

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_all(Awaitables &&...awaitables) {
  return detail::when_all_ready_awaitable<
      std::tuple<detail::when_all_task<awaiter_result_t<Awaitables>>...>>(
      std::make_tuple(
          detail::make_when_all_task(std::forward<Awaitables>(awaitables))...));
}

} // namespace xcoro

// when_all_ready_awaitable 通过 std::tuple<>构造

// detail::when_all_ready_awaitable<std::tuple<detail::when_all_task<typename
// concepts::awaitable_traits<awaitables_type>::awaiter_return_type>...>>(std::make_tuple(detail::make_when_all_task(std::move(awaitables))...))
