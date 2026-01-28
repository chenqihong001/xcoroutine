#pragma once
#include "concepts/awaitable.hpp"
namespace xcoro {
template <concepts::Awaitable T> auto get_awaiter_impl(T &&awaitable) {
  if constexpr (concepts::detail::HasMemberCoAwait<T>) {
    return std::forward<T>(awaitable).operator co_await();
  } else if constexpr (concepts::detail::HasGlobalCoAwait<T>) {
    return operator co_await(std::forward<T>(awaitable));
  } else {
    return std::forward<T>(awaitable);
  }
}

template <concepts::Awaitable T>
using awaiter_t = decltype(get_awaiter_impl(std::declval<T>()));

template <concepts::Awaitable T>
using await_result_t = decltype(std::declval<awaiter_t<T>>().await_resume());

} // namespace xcoro