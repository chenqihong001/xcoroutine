# 协程同步组件

本库提供若干常用协程同步原语，均为 header-only：

- `semaphore`（计数信号量）
- `counting_semaphore`（带上限的计数信号量，标准库语义）
- `binary_semaphore`（单许可信号量）
- `mutex`（协程互斥锁）
- `condition_variable`（协程条件变量）
- `latch`（倒计时门闩）
- `generator`（协程生成器）

实现位置：
- `include/xcoroutine/sync/semaphore.hpp`
- `include/xcoroutine/sync/mutex.hpp`
- `include/xcoroutine/sync/condition_variable.hpp`
- `include/xcoroutine/sync/latch.hpp`
- `include/xcoroutine/generator.hpp`

---

## semaphore

计数信号量，适合做并发限流：

```cpp
xcoro::semaphore sem(2); // 同时允许 2 个进入

auto work = [&]() -> xcoro::task<> {
  co_await sem.acquire();
  // critical section
  sem.release();
  co_return;
};
```

要点：
- `acquire()` 返回 awaiter
- `release(n)` 可一次释放多个
- 支持 `acquire(token)` 取消等待

说明：
- `semaphore` 是 `counting_semaphore<>` 的兼容别名
- `counting_semaphore<LeastMaxValue>::max()` 返回计数上限
- `binary_semaphore` 等价于 `counting_semaphore<1>`

---

## mutex

协程互斥锁，提供 `lock()` awaiter：

```cpp
xcoro::mutex m;

auto work = [&]() -> xcoro::task<> {
  co_await m.lock();
  // critical section
  m.unlock();
  co_return;
};
```

注意：
- **非可重入**
- 由用户负责 `unlock()`
- 支持 `lock(token)` 取消等待

---

## condition_variable

协程条件变量，配合 `xcoro::mutex` 使用：

```cpp
xcoro::mutex m;
xcoro::condition_variable cv;
bool ready = false;

auto waiter = [&]() -> xcoro::task<> {
  co_await m.lock();
  while (!ready) {
    co_await cv.wait(m); // wait 会释放锁，并在唤醒时重新获得锁
  }
  m.unlock();
  co_return;
};

auto notifier = [&]() -> xcoro::task<> {
  co_await m.lock();
  ready = true;
  m.unlock();
  cv.notify_one();
  co_return;
};
```

语义：
- `wait(m)` 会 **释放锁**，通知后再 **重新加锁** 再恢复协程
- `notify_one/notify_all` 恢复一个或全部等待者
- 支持 `wait(m, token)` 取消等待（被取消时会抛 `operation_cancelled`）

---

## latch

倒计时门闩，用于等待多个任务完成：

```cpp
xcoro::latch done(3);

auto worker = [&]() -> xcoro::task<> {
  // do work...
  done.count_down();
  co_return;
};

co_await done.wait(); // 等待 count 归零
```

`latch::wait(token)` 支持取消等待。

---

## generator

协程生成器，按需产出值：

```cpp
xcoro::generator<int> seq() {
  for (int i = 0; i < 3; ++i) {
    co_yield i;
  }
}

for (auto v : seq()) {
  std::cout << v << "\n";
}
```

支持值类型与引用类型（`generator<T>` / `generator<T&>`）。

---

## 使用建议

- 同步组件优先与 `task<>` 搭配使用
- `condition_variable` 需配合 `xcoro::mutex`
- `generator` 适合惰性序列、流式处理
- 若需要可取消等待，传入 `cancellation_token` 并捕获 `operation_cancelled`
