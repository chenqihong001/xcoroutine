# xcoro

`xcoro` 是一个基于 C++20 协程的轻量协程库，设计方向参考了 `cppcoro` / `libcoro`：  
提供可组合的 awaitable 抽象、同步原语、取消机制和基础网络 I/O 能力。

当前仓库以“头文件实现 + 测试驱动”方式维护，适合学习协程模型与构建中小型异步组件。

## Overview

已实现的核心能力：

- Coroutine types
  - `task<T>`
  - `generator<T>`
- Composition
  - `sync_wait(awaitable)`
  - `when_all(awaitables...)`
  - `when_any(awaitables...)`
  - `when_any(range)`
- Cancellation
  - `cancellation_source`
  - `cancellation_token`
  - `cancellation_registration`
  - `operation_cancelled`
- Synchronization primitives
  - `manual_reset_event` / `event`
  - `counting_semaphore` / `semaphore` / `binary_semaphore`
  - `mutex`
  - `condition_variable`
  - `latch`
- Networking (Linux)
  - `net::io_context` (epoll + eventfd)
  - `net::socket`
  - `net::acceptor`
  - `net::resolver`
  - `net::endpoint`
  - `net::byte_buffer`, `mutable_buffer`, `const_buffer`
- Scheduler-like utility
  - `thread_pool`（实验接口）

## Requirements

- C++20 编译器（建议 `GCC 13+` 或同等级 Clang）
- CMake（仓库当前测试配置使用 `3.10+`）
- Linux（网络模块依赖 `epoll/eventfd`）
- `pthread`（编译/链接时建议加 `-pthread`）

## Quick Start

### 1) 最小示例：`task + sync_wait`

```cpp
#include <iostream>

#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

xcoro::task<int> twice(int x) {
  co_return x * 2;
}

int main() {
  int v = xcoro::sync_wait(twice(21));
  std::cout << v << "\n";  // 42
}
```

本地直接编译（仓库根目录）：

```bash
g++ -std=c++20 -O2 -pthread -I./include demo.cpp -o demo
./demo
```

### 2) `when_all`: 并发等待多个任务

```cpp
#include <tuple>
#include <string>

#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

xcoro::task<int> first() { co_return 3; }
xcoro::task<std::string> second() { co_return "ok"; }

int main() {
  auto result = xcoro::sync_wait(xcoro::when_all(first(), second()));
  // result 是 std::tuple<int, std::string>
  return (std::get<0>(result) == 3 && std::get<1>(result) == "ok") ? 0 : 1;
}
```

### 3) `when_any`: 获取最先完成的任务

```cpp
#include <vector>

#include "xcoro/manual_reset_event.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_any.hpp"

xcoro::task<int> fast() {
  co_return 7;
}

xcoro::task<int> slow(xcoro::manual_reset_event& gate) {
  co_await gate;
  co_return 42;
}

int main() {
  xcoro::manual_reset_event gate;
  auto result = xcoro::sync_wait(xcoro::when_any(fast(), slow(gate)));
  // variadic 版本：result.active_index() + result.get<Index>()
  return (result.active_index() == 0 && result.get<0>() == 7) ? 0 : 1;
}
```

## Cancellation

取消模型与 `cppcoro/libcoro` 常见设计一致：`source` 发起取消，`token` 传播取消状态，  
await 点可抛 `operation_cancelled`。

```cpp
#include "xcoro/cancellation_source.hpp"
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

xcoro::task<void> wait_or_cancel(xcoro::semaphore& sem, xcoro::cancellation_token token) {
  co_await sem.acquire(token);  // 若 token 已取消/过程中取消，会抛 operation_cancelled
}

int main() {
  xcoro::semaphore sem(0);
  xcoro::cancellation_source source;
  try {
    xcoro::sync_wait(wait_or_cancel(sem, source.token()));
  } catch (const xcoro::operation_cancelled&) {
    // cancelled
  }
}
```

## Networking (Linux)

`net::io_context` 使用 `epoll + eventfd` 驱动 I/O，提供：

- `schedule()`
- `sleep_for(duration[, token])`
- `async_read_some/read_exact`
- `async_write_all`
- `async_accept`
- `async_connection`

典型使用流程：

1. 创建 `io_context`
2. 调用 `run()` 启动事件循环线程（或 `run_in_current_thread()`）
3. 提交 awaitable 任务（`sync_wait(...)` 或 `spawn(...)`）
4. 调用 `stop()` 停止循环

## Header Map

常用头文件：

- `xcoro/task.hpp`
- `xcoro/sync_wait.hpp`
- `xcoro/when_all.hpp`
- `xcoro/when_any.hpp`
- `xcoro/cancellation_source.hpp`
- `xcoro/cancellation_token.hpp`
- `xcoro/manual_reset_event.hpp`
- `xcoro/semaphore.hpp`
- `xcoro/mutex.hpp`
- `xcoro/condition_variable.hpp`
- `xcoro/latch.hpp`
- `xcoro/generator.hpp`
- `xcoro/thread_pool.hpp`
- `xcoro/net/io_context.hpp`
- `xcoro/net/socket.hpp`
- `xcoro/net/acceptor.hpp`
- `xcoro/net/resolver.hpp`
- `xcoro/net/endpoint.hpp`
- `xcoro/net/buffer.hpp`

## CMake Integration

当前仓库尚未导出安装后的 CMake package/target，推荐先以源码方式引入：

```cmake
include(FetchContent)
FetchContent_Declare(
  xcoro
  GIT_REPOSITORY https://github.com/<your-org>/xcoro.git
  GIT_TAG main
)
FetchContent_MakeAvailable(xcoro)

target_include_directories(your_target PRIVATE ${xcoro_SOURCE_DIR}/include)
target_link_libraries(your_target PRIVATE Threads::Threads)
```

## Build And Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

当前测试覆盖包含：

- `generator`
- `cancellation`
- `semaphore`
- `mutex`
- `condition_variable`
- `latch`
- `manual_reset_event`
- `when_all`
- `when_any`
- `net::io_context/socket`

## Important Notes

### 1) 协程 Lambda 捕获生命周期

请避免“带捕获的临时协程 lambda”直接调用，这会产生悬垂生命周期风险。  
优先使用“无捕获协程 + 显式参数”。

不推荐：

```cpp
tasks.push_back([&]() -> xcoro::task<int> {
  co_await gate;
  co_return 1;
}()); // 捕获对象可能在挂起后已销毁
```

推荐：

```cpp
auto make_task = [](xcoro::manual_reset_event& ev) -> xcoro::task<int> {
  co_await ev;
  co_return 1;
};
tasks.push_back(make_task(gate));
```

### 2) API 稳定性

该项目处于持续迭代阶段，接口命名与行为可能调整。  
若用于生产，请固定 commit 并结合你的场景补充压力测试。

## Roadmap

- 导出正式 CMake target（`install()/find_package()`）
- 增加跨平台 I/O 后端（当前网络模块偏 Linux）
- 补充更多文档与示例（HTTP client/server、超时控制、背压策略）
- 强化并发压力测试与 sanitizer 测试矩阵
