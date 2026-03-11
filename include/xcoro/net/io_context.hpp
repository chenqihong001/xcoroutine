#pragma once
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/task.hpp"
namespace xcoro::net {
enum io_event {
  READ = EPOLLIN,
  WRITE = EPOLLOUT,
  ERROR = EPOLLERR,
  HANGUP = EPOLLHUP,       // 连接完全断开
  RD_HANGUP = EPOLLRDHUP,  // 对端挂起事件，对端关闭了写端
  ONESHOT = EPOLLONESHOT   // 事件触发后会自动从epoll中禁用该文件描述符
};

inline ssize_t write_no_sigpipe(int fd, const void* buffer, size_t count) {
  auto write_with_blocked_sigpipe = [&](int out_fd, const void* out_buffer, size_t out_count) -> ssize_t {
    sigset_t sigpipe_set;
    sigset_t old_set;
    ::sigemptyset(&sigpipe_set);
    ::sigaddset(&sigpipe_set, SIGPIPE);
    if (::pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_set) != 0) {
      return ::write(out_fd, out_buffer, out_count);
    }

    ssize_t n = ::write(out_fd, out_buffer, out_count);
    const int saved_errno = errno;

    if (n == -1 && saved_errno == EPIPE) {
      timespec timeout{0, 0};
      while (::sigtimedwait(&sigpipe_set, nullptr, &timeout) == -1 && errno == EINTR) {
      }
    }

    (void)::pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
    errno = saved_errno;
    return n;
  };

#ifdef MSG_NOSIGNAL
  ssize_t n = ::send(fd, buffer, count, MSG_NOSIGNAL);
  if (n >= 0) {
    return n;
  }
  if (errno == ENOTSOCK) {
    return write_with_blocked_sigpipe(fd, buffer, count);
  }
  if (errno == EPERM) {
    return write_with_blocked_sigpipe(fd, buffer, count);
  }
  return -1;
#endif
  return write_with_blocked_sigpipe(fd, buffer, count);
}

class io_awaiter;
class schedule_awaiter;
class timer_awaiter;

class io_context {
 public:
  io_context();
  ~io_context();
  io_context(const io_context&) = delete;
  io_context& operator=(const io_context&) = delete;
  io_context(io_context&&) = delete;
  io_context& operator=(io_context&&) = delete;
  void run();
  void run_in_current_thread();
  void stop();

  task<> schedule();
  task<size_t> async_read_some(int fd, void* buffer, size_t count);
  task<size_t> async_read_some(int fd, void* buffer, size_t count, cancellation_token token);

  task<size_t> async_read_exact(int fd, void* buffer, size_t count);
  task<size_t> async_read_exact(int fd, void* buffer, size_t count, cancellation_token token);

  task<size_t> async_write_all(int fd, const void* buffer, size_t count);
  task<size_t> async_write_all(int fd, const void* buffer, size_t count, cancellation_token token);

  task<> async_accept(int fd, cancellation_token token = {});
  task<> async_connection(int fd, cancellation_token token = {});

  template <typename T>
  void spawn(task<T> t);

  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d);

  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token);

 private:
  friend class io_awaiter;
  friend class schedule_awaiter;
  friend class timer_awaiter;

  static constexpr uint64_t kWakeTag = 0xffffffffffffffffULL;
  void enqueue_ready(std::coroutine_handle<> h) noexcept;
  void wake() noexcept;
  void event_loop();
  void drain_ready();
  void drain_timers();
  int calc_timeout_ms() noexcept;

  struct timer_state {
    io_context* ctx{};
    std::coroutine_handle<> handle{};
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};
    void resume_once() noexcept {
      if (!completed.exchange(true, std::memory_order_acq_rel)) {
        ctx->enqueue_ready(handle);
      }
    }
  };

  struct timer_item {
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<timer_state> state;
  };
  struct timer_cmp {
    bool operator()(const timer_item& a, const timer_item& b) const noexcept {
      return a.deadline > b.deadline;
    }
  };
  int epoll_fd_{-1};
  int wake_fd_{-1};
  std::jthread loop_thread_;
  std::atomic<bool> stopped_{false};
  std::mutex ready_mtx_;
  std::queue<std::coroutine_handle<>> ready_;
  std::mutex timer_mtx_;
  std::priority_queue<timer_item, std::vector<timer_item>, timer_cmp> timers_;
};

class io_awaiter {
 public:
  io_awaiter(io_context* ctx, int fd, int events, cancellation_token token = {})
      : ctx_(ctx), fd_(fd), events_(events), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume();
  void on_event(uint32_t) noexcept;

 private:
  void resume_once() noexcept;
  io_context* ctx_{};
  int fd_;
  int events_{0};
  std::coroutine_handle<> handle_{};
  int error_{0};
  std::atomic<bool> completed_{false};
  std::atomic<bool> cancelled_{false};
  cancellation_token token_;
  cancellation_registration reg_;
};

class schedule_awaiter {
 public:
  explicit schedule_awaiter(io_context* ctx) : ctx_(ctx) {}
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume() noexcept {}

 private:
  io_context* ctx_{};
};

class timer_awaiter {
 public:
  explicit timer_awaiter(io_context* ctx, std::chrono::steady_clock::time_point deadline,
                         cancellation_token token = {}) : ctx_(ctx), deadline_(deadline), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume();

 private:
  io_context* ctx_{};
  std::chrono::steady_clock::time_point deadline_;
  cancellation_token token_;
  cancellation_registration reg_;
  std::shared_ptr<io_context::timer_state> state_;
};

namespace detail {

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
  explicit detached_task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
  detached_task(detached_task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
  detached_task& operator=(detached_task&& other) noexcept {
    if (this != &other) {
      h_ = std::exchange(other.h_, {});
      // std::exchange 是一个交换的原子操作
    }
    return *this;
  }
  ~detached_task() = default;

 private:
  std::coroutine_handle<promise_type> h_{};
};

}  // namespace detail
inline io_context::io_context() {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
  }
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "eventfd failed");
  }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeTag;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl add wake_fd failed");
  }
}

inline io_context::~io_context() {
  stop();
  if (wake_fd_ != -1) {
    ::close(wake_fd_);
  }
  if (epoll_fd_ != -1) {
    ::close(epoll_fd_);
  }
}

inline void io_context::run() {
  // 如果事件循环线程已经存在，则直接返回，避免创建多个事件循环线程
  if (loop_thread_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  loop_thread_ = std::jthread([this](std::stop_token) { event_loop(); });
}

inline void io_context::run_in_current_thread() {
  if (loop_thread_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  event_loop();
}

inline void io_context::stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  wake();
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
}

inline void io_context::wake() noexcept {
  if (wake_fd_ == -1) {
    return;
  }
  uint64_t one = 1;
  (void)::write(wake_fd_, &one, sizeof(one));
}

inline void io_context::enqueue_ready(std::coroutine_handle<> h) noexcept {
  std::lock_guard<std::mutex> lock(ready_mtx_);
  ready_.push(h);
}

inline int io_context::calc_timeout_ms() noexcept {
  std::lock_guard<std::mutex> lock(timer_mtx_);
  if (timers_.empty()) {
    return -1;
  }
  auto now = std::chrono::steady_clock::now();
  auto deadline = timers_.top().deadline;
  if (deadline <= now) {
    return 0;
  }
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(diff.count());
}

inline void io_context::drain_timers() {
  auto now = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<timer_state>> due;
  {
    std::lock_guard<std::mutex> lock(timer_mtx_);
    while (!timers_.empty() && timers_.top().deadline <= now) {
      due.push_back(timers_.top().state);
      timers_.pop();
    }
  }
  for (auto& s : due) {
    if (s) {
      s->resume_once();
    }
  }
}

inline void io_context::drain_ready() {
  std::queue<std::coroutine_handle<>> q;
  {
    std::lock_guard<std::mutex> lock(ready_mtx_);
    std::swap(q, ready_);
  }
  while (!q.empty()) {
    auto h = q.front();
    q.pop();
    if (h) {
      h.resume();
    }
  }
}

inline void io_context::event_loop() {
  constexpr int kMaxEvents = 64;
  std::array<epoll_event, kMaxEvents> evs{};
  while (!stopped_.load(std::memory_order_relaxed)) {
    int n = ::epoll_wait(epoll_fd_, evs.data(), kMaxEvents, calc_timeout_ms());
    if (n < 0) {
      if (errno == EINTR) {  // 被信号中断，继续执行
        continue;
      }
      break;
    }
    for (int i = 0; i < n; ++i) {
      if (evs[i].data.u64 == kWakeTag) {
        uint64_t buf = 0;
        while (::read(wake_fd_, &buf, sizeof(buf)) > 0) {
        }
        continue;
      }
      auto* aw = reinterpret_cast<io_awaiter*>(evs[i].data.u64);
      if (aw) {
        aw->on_event(evs[i].events);
      }
    }
    drain_timers();
    drain_ready();
  }
  drain_timers();
  drain_ready();
  stopped_.store(true, std::memory_order_relaxed);
}

inline task<> io_context::schedule() {
  co_await schedule_awaiter{this};
}

inline task<size_t> io_context::async_read_some(int fd, void* buffer, size_t count) {
  co_return co_await async_read_some(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_context::async_read_exact(int fd, void* buffer, size_t count) {
  co_return co_await async_read_exact(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_context::async_write_all(int fd, const void* buffer, size_t count) {
  co_return co_await async_write_all(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_context::async_read_some(int fd, void* buffer, size_t count, cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  auto* p = static_cast<std::byte*>(buffer);
  for (;;) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p, count);
    if (n > 0) {
      co_return static_cast<size_t>(n);
    }
    if (n == 0) {
      co_return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::READ, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "read failed");
  }
}

inline task<size_t> io_context::async_read_exact(int fd, void* buffer, size_t count, cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  auto* p = static_cast<std::byte*>(buffer);
  size_t total = 0;
  while (total < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p + total, count - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      co_return total;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::READ, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "read failed");
  }
  co_return total;
}

inline task<size_t> io_context::async_write_all(int fd, const void* buffer, size_t count, cancellation_token token) {
  if (count == 0) {
    co_return 0;
    ;
  }
  auto* p = static_cast<const std::byte*>(buffer);
  size_t written = 0;
  while (written < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = write_no_sigpipe(fd, p + written, count - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::WRITE, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "write failed");
  }
  co_return written;
}

inline task<> io_context::async_accept(int fd, cancellation_token token) {
  co_await io_awaiter{this, fd, io_event::READ, token};
}

inline task<> io_context::async_connection(int fd, cancellation_token token) {
  co_await io_awaiter{this, fd, io_event::WRITE, token};
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
    throw std::system_error{errno, std::system_category(), "getsockopt SO_ERROR failed"};
  }
  if (err != 0) {
    throw std::system_error{err, std::system_category(), "connect failed"};
  }
}

template <typename T>
inline void io_context::spawn(task<T> t) {
  auto starter = [this](task<T> inner) -> detail::detached_task {
    co_await schedule();
    co_await std::move(inner);
  };
  (void)starter(std::move(t));
}

template <typename Rep, typename Period>
inline task<> io_context::sleep_for(std::chrono::duration<Rep, Period> d) {
  const auto deadline = std::chrono::steady_clock::now() + d;
  co_await timer_awaiter{this, deadline};
}

template <typename Rep, typename Period>
inline task<> io_context::sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token) {
  const auto deadline = std::chrono::steady_clock::now() + d;
  co_await timer_awaiter{this, deadline, token};
}

inline void io_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  handle_ = h;
  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    cancelled_.store(true, std::memory_order_release);
    resume_once();
    ctx_->wake();
    return;
  }
  uint32_t ev = static_cast<uint32_t>(events_ | io_event::ONESHOT | io_event::RD_HANGUP);
  epoll_event event{};
  event.events = ev;
  event.data.u64 = reinterpret_cast<uint64_t>(this);

  // 尝试修改，失败则添加
  if (::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_MOD, fd_, &event) == -1) {
    if (errno == ENOENT) {  // 如果错误是不存在
      // 则尝试添加这个文件描述符
      if (::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_ADD, fd_, &event) == -1) {
        error_ = errno;  // 添加也失败，记录错误
      }
    } else {
      error_ = errno;  // 其他错误，直接记录
    }
  }

  if (token_.can_be_cancelled()) {
    reg_ = cancellation_registration(token_, [this]() noexcept {
      cancelled_.store(true, std::memory_order_release);
      ::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
      resume_once();
      ctx_->wake();
    });
  }
  if (error_ != 0) {  // 出现错误
    resume_once();
    ctx_->wake();
  }
}

inline void io_awaiter::await_resume() {
  if (cancelled_.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
    // 请求取消操作，抛出异常
  }
  // 出现错误抛出异常
  if (error_ != 0) {
    throw std::system_error(error_, std::system_category(), "epoll_ctl failed");
  }
}

inline void io_awaiter::on_event(uint32_t) noexcept {
  resume_once();
}

inline void io_awaiter::resume_once() noexcept {
  if (!completed_.exchange(true, std::memory_order_acq_rel)) {
    ctx_->enqueue_ready(handle_);
  }
}

inline void schedule_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  ctx_->enqueue_ready(h);
  ctx_->wake();
}

inline void timer_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  state_ = std::make_shared<io_context::timer_state>();
  state_->ctx = ctx_;
  state_->handle = h;
  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    state_->cancelled.store(true, std::memory_order_release);
    state_->resume_once();
    ctx_->wake();
    return;
  }
  if (token_.can_be_cancelled()) {
    reg_ = cancellation_registration(token_, [state = state_, ctx = ctx_]() noexcept {
      state->cancelled.store(true, std::memory_order_release);
      state->resume_once();
      ctx->wake();
    });
  }

  {
    std::lock_guard<std::mutex> lock(ctx_->timer_mtx_);
    ctx_->timers_.push(io_context::timer_item{deadline_, state_});
  }
  ctx_->wake();
}

inline void timer_awaiter::await_resume() {
  if (state_ && state_->cancelled.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
}

}  // namespace xcoro::net
