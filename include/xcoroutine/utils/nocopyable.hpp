#pragma once
namespace xcoro::detail {
class nocopyable {
public:
  nocopyable() = default;
  ~nocopyable() = default;
  nocopyable(const nocopyable &) = delete;
  nocopyable &operator=(const nocopyable &) = delete;
};
} // namespace xcoro::detail