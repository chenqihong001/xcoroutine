# cancellation 设计与落地指南（xcoro）

本文档不是概念介绍，而是“按步骤可实现”的工程指南。你可以按这里的顺序，一边学习一边把 cancellation 能力继续完善到 `xcoro`。

---

## 1. 先定语义边界

在协程库里，取消要坚持 3 条规则：

1. **取消是请求，不是强杀**
   - `request_cancellation()` 只是发信号。
   - 是否退出由协程在取消点自行决定。

2. **只在取消点响应**
   - 常见取消点：`co_await` 前后、循环检查点、显式 `throw_if_cancellation_requested(token)`。
   - 这保证语义可预期，不会在任意指令点被打断。

3. **结果统一可观测**
   - 当前 `xcoro` 采用抛 `operation_cancelled`。
   - 也可以扩展为 error code / `expected`，但全库要统一。

---

## 2. 当前仓库已经有的能力（可以直接复用）

截至当前代码，`xcoro` 已经有一个可工作的 cancellation 基础层：

- `include/xcoro/cancellation.hpp`
  - `cancellation_source`
  - `cancellation_token`
  - `cancellation_registration`（RAII 回调解绑）
  - `operation_cancelled`
  - `throw_if_cancellation_requested(token)`

- 已接入同步原语：
  - `include/xcoro/semaphore.hpp`
  - `include/xcoro/mutex.hpp`
  - `include/xcoro/condition_variable.hpp`
  - `include/xcoro/latch.hpp`

- 已接入组合器与 IO：
  - `include/xcoro/when_all.hpp`（含 fail-fast 触发 `source.request_cancellation()` 的重载）
  - `include/xcoro/when_any.hpp`（首个完成后请求取消其他任务）
  - `include/xcoro/net/io_scheduler.hpp`（`io_awaiter` / `timer_awaiter` 支持 token）

换句话说：你不是从 0 开始，而是在“已有可用模型”上做增强。

---

## 3. 当前实现模型（建议先吃透）

`cancellation.hpp` 里核心思路是：

1. `cancellation_state` 内部保存：
   - 一个 `cancelled_` 原子标记
   - 一个 callbacks 容器 + 互斥
2. `request_cancellation()`：
   - CAS 保证只成功一次（幂等）
   - 取出并调用所有回调
3. `cancellation_registration`：
   - 构造时注册回调
   - 析构时自动反注册（避免悬挂回调）

这是协程取消机制里最关键的“通知基础设施”。

---

## 4. 通用 cancellable awaiter 模板（推荐统一到所有组件）

你可以把所有等待型 awaiter 写成同一套路，当前同步组件基本已经这么做了：

```cpp
struct awaiter {
  bool await_ready() noexcept {
    // A. 先看是否已取消（快速路径）
    if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
      cancelled_immediate_ = true;
      return true; // 不挂起，直接走 await_resume 抛异常
    }
    // B. 看资源是否就绪（如 try_lock/try_acquire/count==0）
    return try_fast_path();
  }

  bool await_suspend(std::coroutine_handle<> h) noexcept {
    // C. 再次检查取消（与 ready 之间存在竞态窗口）
    if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
      cancelled_immediate_ = true;
      return false; // 立即恢复
    }

    // D. 入等待队列，建立 waiter_state
    state_ = std::make_shared<waiter_state>(h);
    if (!enqueue_waiter(state_)) {
      // 资源在入队前变为可用，不挂起
      state_.reset();
      return false;
    }

    // E. 注册取消回调：抢到“恢复权”的一方恢复协程
    if (token_.can_be_cancelled()) {
      registration_ = cancellation_registration(token_, [state = state_]() noexcept {
        if (!state->try_mark_resumed()) return;
        state->cancelled.store(true, std::memory_order_release);
        state->handle.resume();
      });
    }

    // F. 注册后再做一次取消检查，补上最后窗口
    if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
      if (state_->try_mark_resumed()) {
        state_->cancelled.store(true, std::memory_order_release);
      }
      return false;
    }
    return true;
  }

  void await_resume() {
    if (cancelled_immediate_ ||
        (state_ && state_->cancelled.load(std::memory_order_acquire))) {
      throw operation_cancelled{};
    }
  }
};
```

上面最重要的是 `try_mark_resumed()`：它确保“通知路径”和“取消路径”只有一个能恢复协程。

---

## 5. 竞态处理要点（实现时逐条自查）

### 5.1 必须覆盖的 3 个竞态

1. **事件完成 vs 取消请求 同时发生**
2. **协程已恢复后，取消回调才到达**
3. **registration 析构与回调执行并发**

### 5.2 推荐约束

- 每个等待操作都要有“单次恢复”保证（如 `resumed` / `completed_` 原子位）
- 原子读写至少使用 `acquire/release` 语义
- 回调里不允许抛异常（当前实现里回调异常会 `std::terminate()`，这是合理的）

---

## 6. 网络 + 组合器的取消模型（对齐 Asio / cppcoro）

这一节把 `io_scheduler`、`when_all`、`when_any` 放在一起讲，因为三者的核心都是同一件事：  
**取消只发请求，完成路径和取消路径竞争“单次恢复权”。**

### 6.1 网络 I/O：`io_awaiter` / `timer_awaiter`（贴近 Asio reactor-op）

`io_scheduler` 里的 fd I/O（`async_read_*` / `async_write_*` / `async_accept` / `async_connect`）是经典的 reactor 循环：

1. 先执行系统调用（`read/write/accept/connect`）。
2. `EINTR` 继续重试。
3. `EAGAIN/EWOULDBLOCK` 时 `co_await io_awaiter{..., token}` 挂起等待可读/可写。
4. 每轮循环都在进入阻塞点前 `throw_if_cancellation_requested(token)`。

`io_awaiter::await_suspend()` 的关键步骤（与 Asio 的 reactor operation 很接近）：

1. 保存 `handle_`，先做一次“立即取消”检查；若已取消则标记 `cancelled_` 并进入恢复路径。
2. 用 `epoll_ctl(ADD/MOD)` 注册 `EPOLLONESHOT` 事件。
3. 若 token 可取消，注册 `cancellation_registration` 回调：
   - `cancelled_ = true`
   - `epoll_ctl(..., EPOLL_CTL_DEL, ...)` 删除监听
   - `resume_once()` + `scheduler_->wake()`
4. `epoll_ctl` 失败则记录 `error_` 并走同一恢复路径。

`on_event()`、取消回调、错误路径最终都汇聚到 `resume_once()`；  
`resume_once()` 用 `completed_` 原子位保证“只入 ready 队列一次”，等价于 Asio 的“completion handler 只完成一次”约束。

`await_resume()` 统一出结果：

- `cancelled_ == true` -> 抛 `operation_cancelled`
- `error_ != 0` -> 抛 `std::system_error`

`timer_awaiter` 也是同一个模型，只是把 epoll 事件换成 `timer_state + 小根堆`：

- 取消时标记 `state_->cancelled = true`，然后 `resume_once()`
- `await_resume()` 看到 `cancelled` 就抛 `operation_cancelled`

和 Asio 的对应关系可以这样理解：

- 一致点：取消是协作式；操作最终会“以取消完成”并在安全点恢复协程。
- 当前差异：还没有 Asio 那种细粒度 cancellation type（terminal/partial/total）和 per-op slot 语义，当前是单一 cancel bit。

### 6.2 `when_all`：贴近 cppcoro `when_all_ready` 的“全部完成后汇总”

当前 `when_all` 的结构与 cppcoro 的经典实现非常接近：

1. 每个子 awaitable 被包装成 `when_all_task<R>`，各自 promise 存储“值或异常”。
2. 父级 `when_all_ready_awaitable` 内部持有 `when_all_latch`。
3. `try_await()` 启动所有子任务，父协程按计数器决定是否挂起。
4. 子任务 `final_suspend()` 调用 `latch.notify_awaitable_completed()`；最后一个完成者恢复父协程。

异常语义也贴近 cppcoro：

- 子任务抛异常不会立刻打断 `when_all`，而是先存入对应 promise。
- 父协程在 `await_resume()` 里统一 `rethrow_if_exception()`，再组装 tuple 结果。
- 这意味着默认语义是“等全部结束，再在汇总点观测错误”。

`when_all(cancellation_source&, ...)` 这个重载提供了 fail-fast 信号：

- 每个子任务通过 `make_when_all_task_cancel_on_failure()` 包装。
- 任一子任务抛异常时，先 `source.request_cancellation()`，再把异常继续抛出保存。

注意它仍然是协作式而不是强中断：

- 兄弟任务只有在共享 token 且命中取消点时才会尽快退出。
- 没有检查 token 的任务仍会跑到自然完成；`when_all` 也会继续等它结束。

### 6.3 `when_any`：贴近 Asio `operator||` 的 winner-takes-all

当前 `when_any` 的实现是“首个完成者胜出”模型：

1. 为每个子任务启动一个 `detached_task`。
2. 所有任务共享 `first_completion_tracker`（内部 `atomic<bool> first_completed_`）。
3. 只有第一个 `try_become_first()` 成功的任务可以写入结果/异常并 `notify_event.set()`。
4. 父协程 `co_await notify` 后返回首个结果或重抛首个异常。

`when_any(cancellation_source&, ...)` 的取消行为与 Asio `awaitable_operators::operator||` 非常接近：

- 当 winner 产生（或首个异常产生）后，请求 `source.request_cancellation()`。
- loser 任务收到取消请求后，在各自取消点退出。

这里也有一个明确边界：

- `source.request_cancellation()` 发生在 `when_any(...)` 返回/抛出之后。
- 在请求发出到 loser 观察到 token 之间会有短暂窗口，因此 loser 可能再前进几步。
- 想达到“尽快收敛”的体验，需要所有分支都统一接入同一个 token（尤其是 I/O awaiter 和定时等待）。

---

## 7. 下一步增强建议（你可以自己实现）

下面是建议的“增量式”路线，不会破坏现有功能。

### 阶段 A：给取消增加 reason（低风险）

目标：保留现有 API，同时增加可观测取消原因。

建议 API：

```cpp
enum class cancel_reason {
  user_request,
  timeout,
  parent_cancelled,
  shutdown
};

class cancellation_source {
public:
  bool request_cancellation(cancel_reason reason = cancel_reason::user_request) noexcept;
};

class cancellation_token {
public:
  cancel_reason reason() const noexcept;
};
```

实现建议：

- 在 `cancellation_state` 增加 `std::atomic<cancel_reason> reason_`
- 只有“首次成功取消”的线程写入 reason
- 读取 reason 前先确认 `is_cancellation_requested()==true`

### 阶段 B：加入 `cancellation_scope`（结构化取消）

目标：父协程可统一取消子任务，避免“孤儿协程”。

最小接口示意：

```cpp
class cancellation_scope {
public:
  cancellation_token token() const noexcept;
  bool request_cancellation() noexcept;
  cancellation_source& source() noexcept;
};
```

落地方式：

1. `scope` 内部持有 `cancellation_source`
2. 子任务统一拿 `scope.token()`
3. 在 `when_all/when_any` 包装层里决定何时触发 `scope.request_cancellation()`

### 阶段 C：统一“可取消 awaiter”辅助模板（减少重复代码）

你会发现 `semaphore/mutex/latch/condition_variable` 的取消逻辑很像。  
可以抽一个 `detail::waiter_state_base` 或辅助函数，减少重复并降低竞态 bug 概率。

---

## 8. 建议测试矩阵（强烈建议先写再重构）

最少要覆盖以下测试：

1. **立即取消**
   - await 前 token 已取消，`await_ready()` 直接走异常路径

2. **挂起后取消**
   - 协程进入等待队列后取消，能被恢复并抛 `operation_cancelled`

3. **完成与取消并发**
   - 反复压测确保不会双 resume / 崩溃

4. **registration 生命周期**
   - awaiter 销毁后不会访问悬挂对象

5. **when_all / when_any 传播**
   - 一个子任务失败后，兄弟任务收到取消请求并尽快退出

6. **IO 场景**
   - epoll 等待中取消，监听被正确移除，协程仅恢复一次

---

## 9. 推荐实现顺序（边学边写）

1. 读透 `include/xcoro/cancellation.hpp` 与四个同步原语中的 awaiter
2. 补齐 cancellation 单元测试（先保行为）
3. 加 `cancel_reason`（不改调用方语义）
4. 加 `cancellation_scope` 并先接 `when_all/when_any`
5. 最后做 awaiter 抽象重构（减少重复代码）

这个顺序的好处是：每一步都能独立验证，遇到问题容易回退和定位。

---

## 10. 一个最小使用示例（当前接口）

```cpp
xcoro::cancellation_source src;
auto token = src.token();

auto worker = [&]() -> xcoro::task<> {
  try {
    for (;;) {
      xcoro::throw_if_cancellation_requested(token);
      co_await sched.sleep_for(std::chrono::milliseconds(50), token);
    }
  } catch (const xcoro::operation_cancelled&) {
    co_return;
  }
};

// 在其他位置请求取消
src.request_cancellation();
```

如果你按上面的阶段推进，最终会得到一个“语义清晰、竞态可控、可组合传播”的 cancellation 体系。

---

## 11. 可重写参考代码：`include/xcoro/cancellation.hpp`

下面这份代码是完整可用的 cancellation 基础层（与你仓库当前实现一致思路）。  
你可以直接手敲一遍，再按自己的风格改。

```cpp
#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xcoro {

class cancellation_token;

namespace detail {

class cancellation_state;

class callback_state {
public:
  explicit callback_state(std::function<void()> cb) : callback_(std::move(cb)) {}

  void invoke() noexcept {
    if (!invoked_.exchange(true, std::memory_order_acq_rel)) {
      try {
        callback_();
      } catch (...) {
        std::terminate();
      }
    }
  }

private:
  std::function<void()> callback_;
  std::atomic<bool> invoked_{false};
};

class cancellation_state : public std::enable_shared_from_this<cancellation_state> {
public:
  bool is_cancellation_requested() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

  bool try_add_callback(const std::shared_ptr<callback_state>& cb) {
    if (is_cancellation_requested()) {
      return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (cancelled_.load(std::memory_order_relaxed)) {
      return false;
    }
    callbacks_.push_back(cb);
    return true;
  }

  void remove_callback(const std::shared_ptr<callback_state>& cb) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (*it == cb) {
        callbacks_.erase(it);
        break;
      }
    }
  }

  bool request_cancellation() noexcept {
    bool expected = false;
    if (!cancelled_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return false;
    }

    std::vector<std::shared_ptr<callback_state>> callbacks;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      callbacks.swap(callbacks_);
    }
    // 把回调列表在持锁期间“搬出去”，然后释放锁后再逐个调用
    // - 保证回调只执行一次：callbacks_被清空（swap后为空），后续不会再被执行
    // - 异常/noexcept友好：swap是noexcept，不会在request_cancellation()里抛异常
    // - 降低临界区事件：只在swap时持锁，减少锁竞争
    for (auto& cb : callbacks) {
      cb->invoke();
    }
    return true;
  }

private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::vector<std::shared_ptr<callback_state>> callbacks_;
};

} // namespace detail

class cancellation_registration {
public:
  cancellation_registration() noexcept = default;

  template <typename Callback>
  cancellation_registration(const cancellation_token& token, Callback&& callback) {
    register_callback(token, std::forward<Callback>(callback));
  }

  cancellation_registration(const cancellation_registration&) = delete;
  cancellation_registration& operator=(const cancellation_registration&) = delete;

  cancellation_registration(cancellation_registration&& other) noexcept
      : state_(std::move(other.state_)),
        callback_(std::move(other.callback_)) {}

  cancellation_registration& operator=(cancellation_registration&& other) noexcept {
    if (this != &other) {
      deregister();
      state_ = std::move(other.state_);
      callback_ = std::move(other.callback_);
    }
    return *this;
  }

  ~cancellation_registration() { deregister(); }

  bool is_registered() const noexcept { return static_cast<bool>(state_); }

  void deregister() noexcept {
    if (state_) {
      state_->remove_callback(callback_);
      state_.reset();
      callback_.reset();
    }
  }

private:
  template <typename Callback>
  void register_callback(const cancellation_token& token, Callback&& callback);

  std::shared_ptr<detail::cancellation_state> state_;
  std::shared_ptr<detail::callback_state> callback_;
};

class cancellation_token {
public:
  cancellation_token() noexcept = default;

  bool can_be_cancelled() const noexcept { return static_cast<bool>(state_); }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

  class awaiter;
  auto operator co_await() const noexcept;

private:
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<detail::cancellation_state> state_;
  friend class cancellation_source;
  friend class cancellation_registration;
};

class cancellation_token::awaiter {
public:
  explicit awaiter(cancellation_token token) : token_(std::move(token)) {}

  bool await_ready() const noexcept {
    return !token_.can_be_cancelled() || token_.is_cancellation_requested();
  }

  bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
    awaiting_ = awaiting;
    registration_ =
        cancellation_registration(token_, [this]() noexcept { awaiting_.resume(); });
    return true;
  }

  void await_resume() noexcept {}

private:
  cancellation_token token_;
  cancellation_registration registration_;
  std::coroutine_handle<> awaiting_{};
};

inline auto cancellation_token::operator co_await() const noexcept {
  return awaiter{*this};
}

class cancellation_source {
public:
  cancellation_source()
      : state_(std::make_shared<detail::cancellation_state>()) {}

  cancellation_token token() const noexcept { return cancellation_token{state_}; }

  bool request_cancellation() noexcept {
    return state_ ? state_->request_cancellation() : false;
  }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

private:
  std::shared_ptr<detail::cancellation_state> state_;
};

class operation_cancelled : public std::exception {
public:
  const char* what() const noexcept override { return "operation cancelled"; }
};

inline void throw_if_cancellation_requested(const cancellation_token& token) {
  if (token.is_cancellation_requested()) {
    throw operation_cancelled{};
  }
}

template <typename Callback>
void cancellation_registration::register_callback(
    const cancellation_token& token, Callback&& callback) {
  if (!token.can_be_cancelled()) {
    return;
  }
  auto state = token.state_;
  auto cb_state = std::make_shared<detail::callback_state>(
      std::function<void()>(std::forward<Callback>(callback)));
  if (state->try_add_callback(cb_state)) {
    state_ = std::move(state);
    callback_ = std::move(cb_state);
  } else {
  // 已经request cancellation了
    cb_state->invoke();
  }
}

} // namespace xcoro
```

---

## 附录 A. 可选增强参考：带 `reason/scope` 的替代版 `cancellation.hpp`

下面这份是“增强版完整实现”，定位是**替代**第 11 节里的基础版（不是并存）。  
如果你要重写成带取消原因和 scope 的版本，可以直接按这份写。

```cpp
#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xcoro {

enum class cancel_reason {
  user_request,
  timeout,
  parent_cancelled,
  shutdown
};

class cancellation_token;

namespace detail {

class callback_state {
public:
  explicit callback_state(std::function<void()> cb) : callback_(std::move(cb)) {}

  void invoke() noexcept {
    if (!invoked_.exchange(true, std::memory_order_acq_rel)) {
      try {
        callback_();
      } catch (...) {
        std::terminate();
      }
    }
  }

private:
  std::function<void()> callback_;
  std::atomic<bool> invoked_{false};
};

class cancellation_state {
public:
  bool is_cancellation_requested() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

  cancel_reason reason() const noexcept {
    return reason_.load(std::memory_order_acquire);
  }

  bool try_add_callback(const std::shared_ptr<callback_state>& cb) {
    if (is_cancellation_requested()) {
      return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (cancelled_.load(std::memory_order_relaxed)) {
      return false;
    }
    callbacks_.push_back(cb);
    return true;
  }

  void remove_callback(const std::shared_ptr<callback_state>& cb) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (*it == cb) {
        callbacks_.erase(it);
        break;
      }
    }
  }

  bool request_cancellation(
      cancel_reason r = cancel_reason::user_request) noexcept {
    bool expected = false;
    if (!cancelled_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return false;
    }

    reason_.store(r, std::memory_order_release);

    std::vector<std::shared_ptr<callback_state>> callbacks;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      callbacks.swap(callbacks_);
    }
    for (auto& cb : callbacks) {
      cb->invoke();
    }
    return true;
  }

private:
  std::atomic<bool> cancelled_{false};
  std::atomic<cancel_reason> reason_{cancel_reason::user_request};
  std::mutex mutex_;
  std::vector<std::shared_ptr<callback_state>> callbacks_;
};

} // namespace detail

class cancellation_registration {
public:
  cancellation_registration() noexcept = default;

  template <typename Callback>
  cancellation_registration(const cancellation_token& token, Callback&& callback) {
    register_callback(token, std::forward<Callback>(callback));
  }

  cancellation_registration(const cancellation_registration&) = delete;
  cancellation_registration& operator=(const cancellation_registration&) = delete;

  cancellation_registration(cancellation_registration&& other) noexcept
      : state_(std::move(other.state_)), callback_(std::move(other.callback_)) {}

  cancellation_registration& operator=(cancellation_registration&& other) noexcept {
    if (this != &other) {
      deregister();
      state_ = std::move(other.state_);
      callback_ = std::move(other.callback_);
    }
    return *this;
  }

  ~cancellation_registration() { deregister(); }

  void deregister() noexcept {
    if (state_) {
      state_->remove_callback(callback_);
      state_.reset();
      callback_.reset();
    }
  }

private:
  template <typename Callback>
  void register_callback(const cancellation_token& token, Callback&& callback);

  std::shared_ptr<detail::cancellation_state> state_;
  std::shared_ptr<detail::callback_state> callback_;
};

class cancellation_token {
public:
  cancellation_token() noexcept = default;

  bool can_be_cancelled() const noexcept { return static_cast<bool>(state_); }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

  cancel_reason reason() const noexcept {
    return state_ ? state_->reason() : cancel_reason::user_request;
  }

private:
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<detail::cancellation_state> state_;
  friend class cancellation_source;
  friend class cancellation_registration;
};

class cancellation_source {
public:
  cancellation_source()
      : state_(std::make_shared<detail::cancellation_state>()) {}

  cancellation_token token() const noexcept { return cancellation_token{state_}; }

  bool request_cancellation(
      cancel_reason r = cancel_reason::user_request) noexcept {
    return state_ ? state_->request_cancellation(r) : false;
  }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

  cancel_reason reason() const noexcept {
    return state_ ? state_->reason() : cancel_reason::user_request;
  }

private:
  std::shared_ptr<detail::cancellation_state> state_;
};

class cancellation_scope {
public:
  cancellation_scope() = default;

  cancellation_token token() const noexcept { return source_.token(); }

  bool request_cancellation(
      cancel_reason r = cancel_reason::user_request) noexcept {
    return source_.request_cancellation(r);
  }

  bool is_cancellation_requested() const noexcept {
    return source_.is_cancellation_requested();
  }

  cancel_reason reason() const noexcept { return source_.reason(); }

  cancellation_source& source() noexcept { return source_; }

private:
  cancellation_source source_;
};

class operation_cancelled : public std::exception {
public:
  const char* what() const noexcept override { return "operation cancelled"; }
};

inline void throw_if_cancellation_requested(const cancellation_token& token) {
  if (token.is_cancellation_requested()) {
    throw operation_cancelled{};
  }
}

template <typename Callback>
void cancellation_registration::register_callback(
    const cancellation_token& token, Callback&& callback) {
  if (!token.can_be_cancelled()) {
    return;
  }
  auto state = token.state_;
  auto cb_state = std::make_shared<detail::callback_state>(
      std::function<void()>(std::forward<Callback>(callback)));

  if (state->try_add_callback(cb_state)) {
    state_ = std::move(state);
    callback_ = std::move(cb_state);
  } else {
    cb_state->invoke();
  }
}

} // namespace xcoro
```

提示：如果你采用这个增强版，记得同步更新 `when_all/when_any/io_scheduler` 里调用 `request_cancellation()` 的地方，按需带上 `cancel_reason`。

---

## 12. 可重写参考代码：`when_all / when_any` 的取消传播

这里贴 cancellation 相关的关键实现片段（不是整个文件），你重写时先把这部分做对就够了。

### 12.1 `include/xcoro/when_all.hpp`（失败触发取消）

```cpp
namespace xcoro::detail {

template <concepts::Awaitable Awaitable,
          typename R = awaiter_result_t<Awaitable>>
when_all_task<R> make_when_all_task_cancel_on_failure(
    Awaitable&& awaitable, cancellation_source& source) {
  try {
    if constexpr (std::is_void_v<R>) {
      co_await std::forward<Awaitable>(awaitable);
      co_return;
    } else {
      co_return co_await std::forward<Awaitable>(awaitable);
    }
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

} // namespace xcoro::detail

namespace xcoro {

template <concepts::Awaitable... Awaitables>
[[nodiscard]] auto when_all(cancellation_source& source,
                            Awaitables&&... awaitables) {
  return detail::when_all_ready_awaitable<
      std::tuple<detail::when_all_task<awaiter_result_t<Awaitables>>...>>(
      std::make_tuple(detail::make_when_all_task_cancel_on_failure(
          std::forward<Awaitables>(awaitables), source)...));
}

} // namespace xcoro
```

### 12.2 `include/xcoro/when_any.hpp`（首个完成后取消其他任务）

```cpp
namespace xcoro {

template <concepts::Awaitable... Awaitables>
[[nodiscard]] task<when_any_result<Awaitables...>>
when_any(cancellation_source& source, Awaitables&&... awaitables) {
  try {
    auto result = co_await when_any(std::forward<Awaitables>(awaitables)...);
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>>
when_any(cancellation_source& source, RangeType awaitables) {
  try {
    auto result = co_await when_any(std::forward<RangeType>(awaitables));
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

} // namespace xcoro
```

---

## 13. 可重写参考代码：`io_scheduler` 中的可取消 awaiter

这部分是 IO 取消机制最关键的代码。你重写时优先保证“只能 resume 一次”。

### 13.1 `io_awaiter` 定义

```cpp
class io_awaiter {
public:
  io_awaiter(io_scheduler* scheduler, int fd, int events,
             cancellation_token token = {})
      : scheduler_(scheduler), fd_(fd), events_(events), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  void await_resume();

  void on_event(std::uint32_t events) noexcept;

private:
  void resume_once() noexcept;

  io_scheduler* scheduler_;
  int fd_;
  int events_;
  std::coroutine_handle<> handle_;
  int error_{0};
  std::atomic<bool> completed_{false};
  std::atomic<bool> cancelled_{false};
  cancellation_token token_;
  cancellation_registration registration_;
};
```

### 13.2 `io_awaiter` 实现

```cpp
inline void io_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
  handle_ = handle;

  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    cancelled_.store(true, std::memory_order_release);
    resume_once();
    scheduler_->wake();
    return;
  }

  std::uint32_t ev = static_cast<std::uint32_t>(
      events_ | io_event::ONESHOT | io_event::RD_HANGUUP);
  epoll_event event{};
  event.events = ev;
  event.data.u64 = reinterpret_cast<std::uint64_t>(this);

  if (epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_MOD, fd_, &event) == -1) {
    if (errno == ENOENT) {
      if (epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_ADD, fd_, &event) == -1) {
        error_ = errno;
      }
    } else {
      error_ = errno;
    }
  }

  if (token_.can_be_cancelled()) {
    registration_ = cancellation_registration(
        token_, [this]() noexcept {
          cancelled_.store(true, std::memory_order_release);
          epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
          resume_once();
          scheduler_->wake();
        });
  }

  if (error_ != 0) {
    resume_once();
    scheduler_->wake();
  }
}

inline void io_awaiter::await_resume() {
  if (cancelled_.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
  if (error_ != 0) {
    throw std::system_error(error_, std::system_category(), "epoll_ctl failed");
  }
}

inline void io_awaiter::on_event(std::uint32_t) noexcept {
  resume_once();
}

inline void io_awaiter::resume_once() noexcept {
  if (!completed_.exchange(true, std::memory_order_acq_rel)) {
    scheduler_->enqueue_ready(handle_);
  }
}
```

### 13.3 `timer_awaiter`（sleep_for 的取消）

```cpp
class timer_awaiter {
public:
  timer_awaiter(io_scheduler* scheduler,
                std::chrono::steady_clock::time_point deadline,
                cancellation_token token = {})
      : scheduler_(scheduler), deadline_(deadline), token_(std::move(token)) {}
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  void await_resume();

private:
  io_scheduler* scheduler_;
  std::chrono::steady_clock::time_point deadline_;
  cancellation_token token_;
  cancellation_registration registration_;
  std::shared_ptr<io_scheduler::timer_state> state_;
};

inline void timer_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
  state_ = std::make_shared<io_scheduler::timer_state>();
  state_->scheduler = scheduler_;
  state_->handle = handle;

  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    state_->cancelled.store(true, std::memory_order_release);
    state_->resume_once();
    scheduler_->wake();
    return;
  }

  if (token_.can_be_cancelled()) {
    registration_ = cancellation_registration(
        token_, [state = state_, scheduler = scheduler_]() noexcept {
          state->cancelled.store(true, std::memory_order_release);
          state->resume_once();
          scheduler->wake();
        });
  }

  {
    std::lock_guard<std::mutex> guard(scheduler_->timer_mu_);
    scheduler_->timers_.push(io_scheduler::timer_item{deadline_, state_});
  }
  scheduler_->wake();
}

inline void timer_awaiter::await_resume() {
  if (state_ && state_->cancelled.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
}
```

### 13.4 `sleep_for(token)` 重载

```cpp
template <typename Rep, typename Period>
inline task<> io_scheduler::sleep_for(std::chrono::duration<Rep, Period> duration,
                                      cancellation_token token) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  co_await timer_awaiter{this, deadline, token};
}
```

---

## 14. 你可以按这个顺序重写

1. 先重写 `cancellation.hpp`（基础设施）
2. 再重写 `semaphore/mutex/latch/condition_variable`（同步等待取消）
3. 再接 `when_all/when_any` 的取消传播
4. 最后重写 `io_scheduler` 的 `io_awaiter/timer_awaiter`

如果你完全按这个顺序写，调试成本最低。

---

## 15. 可重写参考代码：同步组件中的取消实现

下面给的是 cancellation 相关的完整实现版本，适合你直接手写复现。

### 15.1 `include/xcoro/semaphore.hpp`

```cpp
#pragma once

#include "xcoroutine/cancellation.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace xcoro {

class semaphore {
public:
  explicit semaphore(std::ptrdiff_t initial_count = 0) : count_(initial_count) {}

  semaphore(const semaphore&) = delete;
  semaphore& operator=(const semaphore&) = delete;

  bool try_acquire() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ > 0) {
      --count_;
      return true;
    }
    return false;
  }

  void release(std::ptrdiff_t n = 1) {
    if (n <= 0) {
      return;
    }
    std::vector<std::coroutine_handle<>> to_resume;
    to_resume.reserve(static_cast<size_t>(n));

    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (std::ptrdiff_t i = 0; i < n; ++i) {
        bool resumed = false;
        
          while (!waiters_.empty()) {
            auto waiter = waiters_.front();
            waiters_.pop();
            if (!waiter || !waiter->try_mark_resumed()) {
              continue;
            }
            to_resume.push_back(waiter->handle);
            resumed = true;
            break;
          }

          if (!resumed) {
            ++count_;
          }
      }
    }

    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }

private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

public:
  struct awaiter {
    awaiter(semaphore& sem, cancellation_token token = {}) noexcept
        : sem_(sem), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return sem_.try_acquire();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!sem_.enqueue_waiter(state_)) {
        state_.reset();
        return false;
      }

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->handle) {
                state->handle.resume();
              }
            });
      }

      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        if (state_->try_mark_resumed()) {
          state_->cancelled.store(true, std::memory_order_release);
        }
        return false;
      }

      return true;
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

  private:
    semaphore& sem_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto acquire() noexcept { return awaiter{*this}; }
  auto acquire(cancellation_token token) noexcept {
    return awaiter{*this, std::move(token)};
  }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ > 0) {
      --count_;
      return false;
    }
    waiters_.push(state);
    return true;
  }

  std::mutex mutex_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace xcoro
```





### 15.2 `include/xcoro/mutex.hpp`

```cpp
#pragma once

#include "xcoroutine/cancellation.hpp"
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

namespace xcoro {

class condition_variable;

class mutex {
public:
  mutex() = default;
  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;

  bool try_lock() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return true;
    }
    return false;
  }

  void unlock() noexcept {
    std::coroutine_handle<> next;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto waiter = waiters_.front();
        waiters_.pop();
        if (!waiter || !waiter->try_mark_resumed()) {
          continue;
        }
        next = waiter->handle;
        break;
      }
      if (!next) {
        locked_ = false;
        return;
      }
    }
    if (next) {
      next.resume();
    }
  }

private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

public:
  struct awaiter {
    awaiter(mutex& m, cancellation_token token = {}) noexcept
        : mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return mutex_.try_lock();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!mutex_.enqueue_waiter(state_)) {
        state_.reset();
        return false;
      }

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->handle) {
                state->handle.resume();
              }
            });
      }

      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        if (state_->try_mark_resumed()) {
          state_->cancelled.store(true, std::memory_order_release);
        }
        return false;
      }

      return true;
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

  private:
    mutex& mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto lock() noexcept { return awaiter{*this}; }
  auto lock(cancellation_token token) noexcept {
    return awaiter{*this, std::move(token)};
  }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return false;
    }
    waiters_.push(state);
    return true;
  }

  void lock_and_resume(std::coroutine_handle<> handle) noexcept {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!locked_) {
        locked_ = true;
      } else {
        auto state = std::make_shared<waiter_state>();
        state->handle = handle;
        waiters_.push(std::move(state));
        return;
      }
    }
    if (handle) {
      handle.resume();
    }
  }

  std::mutex mutex_;
  bool locked_{false};
  std::queue<std::shared_ptr<waiter_state>> waiters_;

  friend class condition_variable;
};

} // namespace xcoro
```

### 15.3 `include/xcoro/condition_variable.hpp`

```cpp
#pragma once

#include "xcoroutine/cancellation.hpp"
#include "xcoroutine/sync/mutex.hpp"
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace xcoro {

class condition_variable {
public:
  condition_variable() = default;
  condition_variable(const condition_variable&) = delete;
  condition_variable& operator=(const condition_variable&) = delete;

private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    mutex* mutex{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

public:
  struct awaiter {
    awaiter(condition_variable& cv, mutex& m, cancellation_token token = {}) noexcept
        : cv_(cv), mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      state_->mutex = &mutex_;
      cv_.enqueue_waiter(state_);
      mutex_.unlock();

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->mutex) {
                state->mutex->lock_and_resume(state->handle);
              }
            });
      }

      return true;
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

  private:
    condition_variable& cv_;
    mutex& mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait(mutex& m) noexcept { return awaiter{*this, m}; }
  auto wait(mutex& m, cancellation_token token) noexcept {
    return awaiter{*this, m, std::move(token)};
  }

  void notify_one() {
    std::shared_ptr<waiter_state> w;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          w.reset();
          continue;
        }
        break;
      }
    }
    if (w && w->mutex) {
      w->mutex->lock_and_resume(w->handle);
    }
  }

  void notify_all() {
    std::vector<std::shared_ptr<waiter_state>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          continue;
        }
        to_resume.push_back(std::move(w));
      }
    }
    for (auto& w : to_resume) {
      if (w && w->mutex) {
        w->mutex->lock_and_resume(w->handle);
      }
    }
  }

private:
  void enqueue_waiter(const std::shared_ptr<waiter_state>& w) {
    std::lock_guard<std::mutex> guard(mutex_);
    waiters_.push(w);
  }

  std::mutex mutex_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace xcoro
```

### 15.4 `include/xcoro/latch.hpp`

```cpp
#pragma once

#include "xcoroutine/cancellation.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace xcoro {

class latch {
public:
  explicit latch(std::ptrdiff_t count) : count_(count) {}

  latch(const latch&) = delete;
  latch& operator=(const latch&) = delete;

  void count_down(std::ptrdiff_t n = 1) {
    if (n <= 0) {
      return;
    }
    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (count_ == 0) {
        return;
      }
      if (n >= count_) {
        count_ = 0;
        while (!waiters_.empty()) {
          auto waiter = waiters_.front();
          waiters_.pop();
          if (!waiter || !waiter->try_mark_resumed()) {
            continue;
          }
          to_resume.push_back(waiter->handle);
        }
      } else {
        count_ -= n;
      }
    }
    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }

  bool try_wait() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return count_ == 0;
  }

private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

public:
  struct awaiter {
    awaiter(latch& l, cancellation_token token = {}) noexcept
        : latch_(l), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return latch_.try_wait();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!latch_.enqueue_waiter(state_)) {
        state_.reset();
        return false;
      }

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->handle) {
                state->handle.resume();
              }
            });
      }

      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        if (state_->try_mark_resumed()) {
          state_->cancelled.store(true, std::memory_order_release);
        }
        return false;
      }

      return true;
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

  private:
    latch& latch_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait() noexcept { return awaiter{*this}; }
  auto wait(cancellation_token token) noexcept {
    return awaiter{*this, std::move(token)};
  }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state>& state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == 0) {
      return false;
    }
    waiters_.push(state);
    return true;
  }

  mutable std::mutex mutex_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace xcoro
```
