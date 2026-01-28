#pragma once
#include "concepts/awaitable.hpp"
namespace xcoro {
namespace detail {
template <concepts::Awaitable T> auto get_awaiter_impl(T &&awaitable) {
  if constexpr (concepts::detail::HasMemberCoAwait<T>) {
    // std::cout << "HasMemberCoAwait" << '\n';
    return std::forward<T>(awaitable).operator co_await();
  } else if constexpr (concepts::detail::HasGlobalCoAwait<T>) {
    // std::cout << "HasGlobalCoAwait" << '\n';
    return operator co_await(std::forward<T>(awaitable));
  } else {
    // std::cout << "NoCoAwait" << '\n';
    return std::forward<T>(awaitable);
  }
}

} // namespace detail

template <concepts::Awaitable T>
using awaiter_t = decltype(detail::get_awaiter_impl(std::declval<T>()));

template <typename T>
using await_result_t = decltype(std::declval<awaiter_t<T>>().await_resume());

} // namespace xcoro