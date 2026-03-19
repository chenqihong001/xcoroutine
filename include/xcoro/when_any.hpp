#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "awaitable_traits.hpp"
#include "cancellation_source.hpp"
#include "manual_reset_event.hpp"
#include "task.hpp"
#include "void_value.hpp"

namespace xcoro {

template <typename... Awaitables>
struct when_any_result;

namespace detail {

template <typename R>
struct when_any_storage {
  using value_type = std::conditional_t<std::is_reference_v<R>,
                                        std::add_pointer_t<std::remove_reference_t<R>>,
                                        std::remove_const_t<R>>;
  // 如果R是普通类型，比如int,string就真的把值存下来
  // 如果R是引用类型，比如int&,const string&那就改存一个指针

  // 为什么要去掉const，因为“按值存储”时，顶层的const通常没有太大意义，反而会让容器变得不方便移动，赋值
  value_type value{};

  // R&& value表示消费一个值，你给我一个右值，我直接把它move进storage
  static when_any_storage from(R&& value) {
    if constexpr (std::is_reference_v<R>) {
      return when_any_storage{std::addressof(value)};
    } else {
      return when_any_storage{std::forward<R>(value)};
    }
  }

  // get函数把这个内部表示恢复成外部看到的R
  decltype(auto) get() & {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return value;
    }
  }

  decltype(auto) get() const& {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return static_cast<const value_type&>(value);
    }
  }

  decltype(auto) get() && {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return std::move(value);
    }
  }
};

// 特化void版本的返回值存储
template <>
struct when_any_storage<void> {
  using value_type = void_value;
  value_type value{};

  static when_any_storage from() { return {}; }

  void get() & noexcept {}
  void get() const& noexcept {}
  void get() && noexcept {}
};

struct detached_task {
  struct promise_type {
    detached_task get_return_object() noexcept {
      return detached_task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };

  explicit detached_task(std::coroutine_handle<promise_type>) noexcept {}
};

// 多个异步任务竞争时，仅允许第一个完成者写入结果或异常
template <typename Result>
class completion_state {
 public:
  bool try_publish_result(Result&& result) {
    if (!try_claim_first()) {
      return false;
    }
    result_.emplace(std::move(result));
    return true;
  }

  bool try_publish_exception(std::exception_ptr exception) noexcept {
    if (!try_claim_first()) {
      return false;
    }
    exception_ = std::move(exception);
    return true;
  }

  bool has_exception() const noexcept { return static_cast<bool>(exception_); }

  std::exception_ptr exception() const noexcept { return exception_; }

  // 消费方
  Result take_result() { return std::move(result_.value()); }

 private:
  // 尝试标记第一个完成
  bool try_claim_first() noexcept {
    bool expected = false;
    if (!completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      return false;
    }
    return true;
  }

  std::atomic<bool> completed_{false};
  std::optional<Result> result_;
  std::exception_ptr exception_;
};

// 把一个 awaitable 包装成独立协程，等待其完成后尝试发布结果
template <typename Result, std::size_t Index, concepts::Awaitable Awaitable>
detached_task spawn_when_any_task(Awaitable awaitable,
                                  std::shared_ptr<completion_state<Result>> state,
                                  std::shared_ptr<manual_reset_event> notify_event) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    using storage_t = when_any_storage<await_result_t>;

    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::move(awaitable);
      auto result = Result{
          typename Result::variant_type{std::in_place_index<Index>, storage_t::from()}};
      if (state->try_publish_result(std::move(result))) {
        notify_event->set();
      }
    } else {
      decltype(auto) value = co_await std::move(awaitable);
      auto result = Result{
          typename Result::variant_type{std::in_place_index<Index>,
                                        storage_t::from(std::forward<decltype(value)>(value))}};
      if (state->try_publish_result(std::move(result))) {
        notify_event->set();
      }
    }
  } catch (...) {
    if (state->try_publish_exception(std::current_exception())) {
      notify_event->set();
    }
  }
}

template <typename Result, concepts::Awaitable Awaitable>
detached_task spawn_when_any_range_task(Awaitable awaitable,
                                        std::shared_ptr<completion_state<Result>> state,
                                        std::shared_ptr<manual_reset_event> notify_event) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    using storage_t = when_any_storage<await_result_t>;

    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::move(awaitable);
      auto result = Result{storage_t::from()};
      if (state->try_publish_result(std::move(result))) {
        notify_event->set();
      }
    } else {
      decltype(auto) value = co_await std::move(awaitable);
      auto result = Result{storage_t::from(std::forward<decltype(value)>(value))};
      if (state->try_publish_result(std::move(result))) {
        notify_event->set();
      }
    }
  } catch (...) {
    if (state->try_publish_exception(std::current_exception())) {
      notify_event->set();
    }
  }
}

template <typename Result, typename Tuple, std::size_t... Indices>
void spawn_when_any_tasks_from_tuple(
    Tuple& awaitables,
    const std::shared_ptr<completion_state<Result>>& state,
    const std::shared_ptr<manual_reset_event>& notify_event,
    std::index_sequence<Indices...>) {
  (spawn_when_any_task<Result, Indices>(
       std::move(std::get<Indices>(awaitables)), state, notify_event),
   ...);
}

// 变参版本when_any的核心实现函数
template <typename... Awaitables>
task<when_any_result<Awaitables...>> when_any_from_tuple(
    std::tuple<Awaitables...> awaitables) {
  using result_t = when_any_result<Awaitables...>;

  auto state = std::make_shared<completion_state<result_t>>();
  auto notify = std::make_shared<manual_reset_event>();

  spawn_when_any_tasks_from_tuple<result_t>(
      awaitables, state, notify, std::index_sequence_for<Awaitables...>{});

  co_await *notify;
  if (state->has_exception()) {
    std::rethrow_exception(state->exception());
  }
  co_return state->take_result();
}

template <typename... Awaitables>
task<when_any_result<Awaitables...>> when_any_from_tuple_with_cancellation(
    cancellation_source& source,
    std::tuple<Awaitables...> awaitables) {
  try {
    auto result = co_await when_any_from_tuple<Awaitables...>(std::move(awaitables));
    source.request_cancellation();
    // 正常返回结果
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

}  // namespace detail

// 包装variant
template <typename... Awaitables>
struct when_any_result {
  using variant_type =
      std::variant<detail::when_any_storage<awaiter_result_t<Awaitables>>...>;

  variant_type result;

  std::size_t active_index() const noexcept { return result.index(); }

  template <std::size_t Index>
  bool holds() const noexcept {
    return result.index() == Index;
  }

  template <std::size_t Index>
  decltype(auto) get() & {
    return std::get<Index>(result).get();
  }

  template <std::size_t Index>
  decltype(auto) get() const& {
    return std::get<Index>(result).get();
  }

  template <std::size_t Index>
  decltype(auto) get() && {
    return std::get<Index>(std::move(result)).get();
  }
};

template <concepts::Awaitable Awaitable>
struct when_any_range_result {
  using storage_type = detail::when_any_storage<awaiter_result_t<Awaitable>>;

  storage_type result;

  decltype(auto) get() & { return result.get(); }
  decltype(auto) get() const& { return result.get(); }
  decltype(auto) get() && { return std::move(result).get(); }
};

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_any(Awaitables&&... awaitables)
    -> task<when_any_result<std::decay_t<Awaitables>...>> {
  static_assert(sizeof...(Awaitables) > 0, "when_any requires at least one awaitable");

  return detail::when_any_from_tuple<std::decay_t<Awaitables>...>(
      std::make_tuple(std::forward<Awaitables>(awaitables)...));
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>> when_any(RangeType awaitables) {
  using result_t = when_any_range_result<Awaitable>;
  auto state = std::make_shared<detail::completion_state<result_t>>();
  auto notify = std::make_shared<manual_reset_event>();

  bool has_awaitable = false;
  for (auto& awaitable : awaitables) {
    has_awaitable = true;
    detail::spawn_when_any_range_task<result_t>(std::move(awaitable), state, notify);
  }

  if (!has_awaitable) {
    throw std::runtime_error("when_any requires at least one awaitable");
  }

  co_await *notify;
  if (state->has_exception()) {
    std::rethrow_exception(state->exception());
  }
  co_return state->take_result();
}

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_any(cancellation_source& source,
                            Awaitables&&... awaitables)
    // std::decay_t消除引用
    -> task<when_any_result<std::decay_t<Awaitables>...>> {
  return detail::when_any_from_tuple_with_cancellation<std::decay_t<Awaitables>...>(
      source, std::make_tuple(std::forward<Awaitables>(awaitables)...));
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>> when_any(cancellation_source& source,
                                                              RangeType awaitables) {
  try {
    auto result = co_await when_any(std::move(awaitables));
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

}  // namespace xcoro
