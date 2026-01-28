#pragma once
#include "utils/nocopyable.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace xcoro {

namespace detail {

class sync_wait_event : public nocopyable {
public:
  sync_wait_event(bool initially_set = false) : is_set_(initially_set) {}
  ~sync_wait_event() = default;

  void set() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    is_set_.exchange(true);
    cv_.notify_all();
  }

  void reset() noexcept { is_set_.exchange(false); }
  void wait() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]() { return is_set_.load(); });
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic_bool is_set_ = false;
};

class sync_wait_task_promise final {};

} // namespace detail

} // namespace xcoro