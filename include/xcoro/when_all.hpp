#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "awaitable_traits.hpp"
#include "cancellation_source.hpp"
#include "void_value.hpp"

namespace xcoro {

namespace detail {

class when_all_latch {
 public:
  explicit when_all_latch(std::size_t count) noexcept : count_(count + 1) {}

  bool is_ready() const noexcept {
    return count_.load(std::memory_order_acquire) == 1;
  }

  void set_awaiting_coroutine(std::coroutine_handle<> awaiting_coroutine) noexcept {
    awaiting_coroutine_ = awaiting_coroutine;
  }

  bool try_await() noexcept {
    return count_.fetch_sub(1, std::memory_order_acq_rel) > 1;
  }

  void notify_awaitable_completed() noexcept {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (awaiting_coroutine_) {
        awaiting_coroutine_.resume();
      }
    }
  }

 private:
  std::atomic<std::size_t> count_;
  std::coroutine_handle<> awaiting_coroutine_;
};

template <typename T>
class when_all_task;

template <typename TaskTuple>
class when_all_ready_awaitable;

template <>
class when_all_ready_awaitable<std::tuple<>> {
 public:
  when_all_ready_awaitable() noexcept = default;
  explicit when_all_ready_awaitable(std::tuple<>) noexcept {}

  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) noexcept {}
  std::tuple<> await_resume() noexcept { return {}; }
};

template <typename... Task>
class when_all_ready_awaitable<std::tuple<Task...>> {
 private:
  template <typename TaskType>
  using task_promise_t = typename std::remove_reference_t<TaskType>::promise_type;

  template <typename TaskType>
  using task_value_t = typename task_promise_t<TaskType>::value_type;

  using lvalue_result_tuple_t = std::tuple<task_value_t<Task>&...>;
  using rvalue_result_tuple_t = std::tuple<task_value_t<Task>...>;

  template <bool MoveResults>
  class awaiter {
   public:
    explicit awaiter(when_all_ready_awaitable& awaitable) noexcept
        : awaitable_(awaitable) {}

    bool await_ready() const noexcept { return awaitable_.is_ready(); }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      return awaitable_.prepare_and_try_suspend(handle);
    }

    auto await_resume() {
      awaitable_.throw_if_any_exception();
      if constexpr (MoveResults) {
        return awaitable_.collect_results_rvalue();
      } else {
        return awaitable_.collect_results_lvalue();
      }
    }

   private:
    when_all_ready_awaitable& awaitable_;
  };

 public:
  explicit when_all_ready_awaitable(Task&&... tasks) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<Task>...>)
      : latch_(sizeof...(Task)), tasks_(std::forward<Task>(tasks)...) {}

  explicit when_all_ready_awaitable(std::tuple<Task...>&& tasks) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<Task>...>)
      : latch_(sizeof...(Task)), tasks_(std::move(tasks)) {}

  auto operator co_await() & noexcept { return awaiter<false>{*this}; }
  auto operator co_await() && noexcept { return awaiter<true>{*this}; }

 private:
  bool is_ready() const noexcept { return latch_.is_ready(); }

  bool prepare_and_try_suspend(std::coroutine_handle<> handle) noexcept {
    latch_.set_awaiting_coroutine(handle);
    std::apply([this](auto&... tasks) { (tasks.start(latch_), ...); }, tasks_);
    return latch_.try_await();
  }

  void throw_if_any_exception() {
    std::apply([](auto&... tasks) { (tasks.promise().rethrow_if_exception(), ...); }, tasks_);
  }

  lvalue_result_tuple_t collect_results_lvalue() {
    return std::apply(
        [](auto&... tasks) { return lvalue_result_tuple_t{tasks.promise().result()...}; },
        tasks_);
  }

  rvalue_result_tuple_t collect_results_rvalue() {
    return std::apply(
        [](auto&... tasks) {
          return rvalue_result_tuple_t{std::move(tasks).promise().result()...};
        },
        tasks_);
  }

  when_all_latch latch_;
  std::tuple<Task...> tasks_;
};

template <typename R>
class when_all_task_promise {
 public:
  using value_type = std::conditional_t<std::is_reference_v<R>, R, std::remove_const_t<R>>;
  using storage_type = std::conditional_t<std::is_reference_v<R>,
                                          std::add_pointer_t<std::remove_reference_t<R>>,
                                          std::remove_const_t<R>>;

  when_all_task_promise() noexcept = default;

  when_all_task<R> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<when_all_task_promise> handle) noexcept {
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
    requires std::is_constructible_v<R, U&&>
  void return_value(U&& value) noexcept {
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

  void start(when_all_latch& latch) noexcept {
    latch_ = &latch;
    std::coroutine_handle<when_all_task_promise>::from_promise(*this).resume();
  }

 private:
  when_all_latch* latch_ = nullptr;
  std::variant<std::monostate, storage_type, std::exception_ptr> result_;
};

template <>
class when_all_task_promise<void> {
 public:
  using value_type = void_value;

  when_all_task_promise() noexcept = default;

  when_all_task<void> get_return_object() noexcept;
  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct completion_notifier {
      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<when_all_task_promise> handle) noexcept {
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

  value_type& result() & {
    rethrow_if_exception();
    return value_;
  }

  value_type&& result() && {
    rethrow_if_exception();
    return std::move(value_);
  }

  void start(when_all_latch& latch) noexcept {
    latch_ = &latch;
    std::coroutine_handle<when_all_task_promise>::from_promise(*this).resume();
  }

 private:
  when_all_latch* latch_ = nullptr;
  std::exception_ptr exception_;
  value_type value_{};
};

template <typename R>
class when_all_task {
 public:
  using promise_type = when_all_task_promise<R>;

  explicit when_all_task(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  when_all_task(when_all_task&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  when_all_task& operator=(when_all_task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  when_all_task(const when_all_task&) = delete;
  when_all_task& operator=(const when_all_task&) = delete;

  ~when_all_task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  void start(when_all_latch& latch) noexcept { handle_.promise().start(latch); }

  promise_type& promise() & noexcept { return handle_.promise(); }
  promise_type&& promise() && noexcept { return std::move(handle_.promise()); }

 private:
  std::coroutine_handle<promise_type> handle_;
};

template <typename T>
when_all_task<T> when_all_task_promise<T>::get_return_object() noexcept {
  return when_all_task<T>{std::coroutine_handle<when_all_task_promise<T>>::from_promise(*this)};
}

inline when_all_task<void> when_all_task_promise<void>::get_return_object() noexcept {
  return when_all_task<void>{
      std::coroutine_handle<when_all_task_promise<void>>::from_promise(*this)};
}

template <concepts::Awaitable Awaitable,
          typename R = awaiter_result_t<Awaitable>>
when_all_task<R> make_when_all_task(Awaitable&& awaitable) {
  if constexpr (std::is_void_v<R>) {
    co_await std::forward<Awaitable>(awaitable);
    co_return;
  } else {
    co_return co_await std::forward<Awaitable>(awaitable);
  }
}

template <concepts::Awaitable Awaitable,
          typename R = awaiter_result_t<Awaitable>>
when_all_task<R> make_when_all_task_cancel_on_failure(Awaitable&& awaitable,
                                                      cancellation_source& source) {
  try {
    if constexpr (std::is_void_v<R>) {
      co_await std::forward<Awaitable>(awaitable);
      co_return;
    } else {
      co_return co_await std::forward<Awaitable>(awaitable);
    }
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

}  // namespace detail

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_all(Awaitables&&... awaitables) {
  return detail::when_all_ready_awaitable<
      std::tuple<detail::when_all_task<awaiter_result_t<Awaitables>>...>>(
      std::make_tuple(detail::make_when_all_task(std::forward<Awaitables>(awaitables))...));
}

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_all(cancellation_source& source,
                            Awaitables&&... awaitables) {
  return detail::when_all_ready_awaitable<
      std::tuple<detail::when_all_task<awaiter_result_t<Awaitables>>...>>(
      std::make_tuple(detail::make_when_all_task_cancel_on_failure(
          std::forward<Awaitables>(awaitables), source)...));
}

}  // namespace xcoro

