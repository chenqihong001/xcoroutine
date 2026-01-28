#pragma once
#include <concepts>
#include <coroutine>
namespace xcoro::concepts {

template <typename T>
concept Awaiter = requires(T t, std::coroutine_handle<> h) {
  { t.await_ready() } -> std::convertible_to<bool>;
  requires std::same_as<decltype(t.await_suspend(h)), void> ||
               std::same_as<decltype(t.await_suspend(h)), bool> ||
               std::same_as<decltype(t.await_suspend(h)),
                            std::coroutine_handle<>>;
  { t.await_resume() };
};

namespace detail {
template <typename T>
concept HasMemberCoAwait = requires(T t) {
  { t.operator co_await() } -> Awaiter;
};

template <typename T>
concept HasGlobalCoAwait = requires(T t) {
  { operator co_await(t) } -> Awaiter;
};

} // namespace detail

template <typename T>
concept Awaitable =
    detail::HasMemberCoAwait<T> || detail::HasGlobalCoAwait<T> || Awaiter<T>;

} // namespace xcoro::concepts