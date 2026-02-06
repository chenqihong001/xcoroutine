#pragma once
namespace xcoro::detail {
struct void_value {};

// 辅助函数：将value_type转换为T
// 对于void类型，忽略void_value参数
template <typename T> T &&unwrap_void_value(T &&value) noexcept {
  return static_cast<T &&>(value);
}

template <typename T> T &unwrap_void_value(T &value) noexcept {
  return value;
}

// void特化：丢弃void_value，不返回任何值
inline void unwrap_void_value(void_value &&) noexcept {}
inline void unwrap_void_value(void_value &) noexcept {}

} // namespace xcoro::detail