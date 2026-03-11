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

  value_type value{};

  static when_any_storage from(R&& value) {
    if constexpr (std::is_reference_v<R>) {
      return when_any_storage{std::addressof(value)};
    } else {
      return when_any_storage{std::forward<R>(value)};
    }
  }

  decltype(auto) get() & {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return (value);
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

template <>
struct when_any_storage<void> {
  using value_type = void_value;
  value_type value{};

  static when_any_storage from() { return {}; }

  void get() & noexcept {}
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

template <typename Result>
class completion_state {
 public:
  bool try_publish_result(std::size_t index, Result&& result) {
    if (!try_claim_first()) {
      return false;
    }
    first_index_ = index;
    result_.emplace(std::move(result));
    return true;
  }

  bool try_publish_exception(std::size_t index, std::exception_ptr exception) noexcept {
    if (!try_claim_first()) {
      return false;
    }
    first_index_ = index;
    exception_ = std::move(exception);
    return true;
  }

  bool has_exception() const noexcept { return static_cast<bool>(exception_); }

  std::exception_ptr exception() const noexcept { return exception_; }

  Result take_result() { return std::move(result_.value()); }

 private:
  bool try_claim_first() noexcept {
    bool expected = false;
    if (!completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      return false;
    }
    return true;
  }

  std::atomic<bool> completed_{false};
  std::size_t first_index_{static_cast<std::size_t>(-1)};
  std::optional<Result> result_;
  std::exception_ptr exception_;
};

template <typename Result, typename MakeResult>
void publish_result_if_first(const std::shared_ptr<completion_state<Result>>& state,
                             manual_reset_event& notify_event,
                             std::size_t index,
                             MakeResult&& make_result) {
  if (state->try_publish_result(index, make_result())) {
    notify_event.set();
  }
}

template <typename Result, std::size_t Index, concepts::Awaitable Awaitable>
detached_task spawn_when_any_task(Awaitable awaitable,
                                  std::shared_ptr<completion_state<Result>> state,
                                  manual_reset_event& notify_event) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::move(awaitable);
      publish_result_if_first<Result>(state, notify_event, Index, [] {
        using holder_t = when_any_storage<void>;
        return Result{Index, typename Result::variant_type{std::in_place_index<Index>, holder_t::from()}};
      });
    } else {
      decltype(auto) value = co_await std::move(awaitable);
      publish_result_if_first<Result>(state, notify_event, Index, [&]() {
        using holder_t = when_any_storage<await_result_t>;
        return Result{
            Index,
            typename Result::variant_type{std::in_place_index<Index>,
                                          holder_t::from(std::forward<decltype(value)>(value))}};
      });
    }
  } catch (...) {
    if (state->try_publish_exception(Index, std::current_exception())) {
      notify_event.set();
    }
  }
}

template <typename Result, concepts::Awaitable Awaitable>
detached_task spawn_when_any_range_task(Awaitable awaitable,
                                        std::shared_ptr<completion_state<Result>> state,
                                        manual_reset_event& notify_event,
                                        std::size_t index) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::move(awaitable);
      publish_result_if_first<Result>(state, notify_event, index,
                                      [index] { return Result{index, when_any_storage<void>::from()}; });
    } else {
      decltype(auto) value = co_await std::move(awaitable);
      publish_result_if_first<Result>(state, notify_event, index, [&]() {
        return Result{index,
                      when_any_storage<await_result_t>::from(std::forward<decltype(value)>(value))};
      });
    }
  } catch (...) {
    if (state->try_publish_exception(index, std::current_exception())) {
      notify_event.set();
    }
  }
}

template <typename Result, typename... Awaitables, std::size_t... Indices>
void spawn_when_any_variadic_impl(std::index_sequence<Indices...>,
                                  const std::shared_ptr<completion_state<Result>>& state,
                                  manual_reset_event& notify_event,
                                  Awaitables&&... awaitables) {
  (spawn_when_any_task<Result, Indices, Awaitables>(std::forward<Awaitables>(awaitables), state,
                                                     notify_event),
   ...);
}

template <typename Result, typename... Awaitables>
void spawn_when_any_variadic(const std::shared_ptr<completion_state<Result>>& state,
                             manual_reset_event& notify_event,
                             Awaitables&&... awaitables) {
  spawn_when_any_variadic_impl<Result>(std::index_sequence_for<Awaitables...>{}, state,
                                       notify_event, std::forward<Awaitables>(awaitables)...);
}

template <typename... Awaitables>
task<when_any_result<Awaitables...>> when_any_variadic_owned(
    std::tuple<Awaitables...> awaitables) {
  using result_t = when_any_result<Awaitables...>;

  auto state = std::make_shared<completion_state<result_t>>();
  manual_reset_event notify;

  std::apply(
      [&](auto&... owned_awaitables) {
        spawn_when_any_variadic<result_t>(state, notify,
                                          std::move(owned_awaitables)...);
      },
      awaitables);

  co_await notify;
  if (state->has_exception()) {
    std::rethrow_exception(state->exception());
  }
  co_return state->take_result();
}

template <typename... Awaitables>
task<when_any_result<Awaitables...>> when_any_variadic_owned_with_cancellation(
    cancellation_source& source,
    std::tuple<Awaitables...> awaitables) {
  try {
    auto result = co_await when_any_variadic_owned<Awaitables...>(std::move(awaitables));
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

}  // namespace detail

template <typename... Awaitables>
struct when_any_result {
  using variant_type =
      std::variant<detail::when_any_storage<awaiter_result_t<Awaitables>>...>;

  std::size_t index{};
  variant_type result;

  template <std::size_t Index>
  decltype(auto) get() & {
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

  std::size_t index{};
  storage_type result;

  decltype(auto) get() & { return result.get(); }
  decltype(auto) get() && { return std::move(result).get(); }
};

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_any(Awaitables&&... awaitables)
    -> task<when_any_result<std::decay_t<Awaitables>...>> {
  static_assert(sizeof...(Awaitables) > 0, "when_any requires at least one awaitable");

  return detail::when_any_variadic_owned<std::decay_t<Awaitables>...>(
      std::make_tuple(std::forward<Awaitables>(awaitables)...));
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>> when_any(RangeType awaitables) {
  using result_t = when_any_range_result<Awaitable>;
  auto state = std::make_shared<detail::completion_state<result_t>>();
  manual_reset_event notify;

  std::size_t index = 0;
  for (auto& awaitable : awaitables) {
    detail::spawn_when_any_range_task<result_t>(std::move(awaitable), state, notify, index);
    ++index;
  }

  if (index == 0) {
    throw std::runtime_error("when_any requires at least one awaitable");
  }

  co_await notify;
  if (state->has_exception()) {
    std::rethrow_exception(state->exception());
  }
  co_return state->take_result();
}

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_any(cancellation_source& source,
                            Awaitables&&... awaitables)
    -> task<when_any_result<std::decay_t<Awaitables>...>> {
  return detail::when_any_variadic_owned_with_cancellation<std::decay_t<Awaitables>...>(
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
