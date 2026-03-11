#pragma once

namespace xcoro {

struct void_value {
  constexpr bool operator==(const void_value&) const noexcept = default;
};

}  // namespace xcoro

