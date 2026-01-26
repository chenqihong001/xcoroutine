# C++20 协程详解

## 概述

C++20 引入了协程作为语言特性，提供了一种轻量级的异步编程方式。协程可以被暂停和恢复，使得异步代码看起来像同步代码一样直观。

## 协程执行流程

协程的生命周期由编译器自动管理，以下是编译器展开的伪代码：

```cpp
// 编译器展开伪代码：
{
    // 1. 分配协程帧
    auto frame = allocate_coroutine_frame();
    
    // 2. 构造promise对象
    auto& promise = construct_promise(frame);
    
    // 3. 获取返回对象
    task<int> result = promise.get_return_object();
    
    // 4. 初始挂起点
    if (promise.initial_suspend().await_ready()) {
        // 如果不需要挂起，立即执行协程体
    } else {
        // 挂起协程，返回给调用者
        return result;
    }
    
    // 5. 执行协程体
    try {
        std::cout << "Step 1\n";
        co_await something();  // 可能挂起点
        std::cout << "Step 2\n";
        promise.return_value(42);  // 或 co_return 42
    } catch (...) {
        promise.unhandled_exception();
    }
    
    // 6. 最终挂起点
    if(promise.final_suspend().await_ready()) {
        // 协程结束，清理资源
    } else {
        // 协程挂起，等待外部恢复
    }
}
```

### 执行流程详解

1. **协程帧分配**：编译器为协程分配存储空间，包含局部变量、promise对象等
2. **Promise构造**：创建promise对象，控制协程行为
3. **返回对象创建**：通过`get_return_object()`创建返回给调用者的对象
4. **初始挂起**：决定协程是立即执行还是先挂起
5. **协程体执行**：执行用户编写的协程代码
6. **最终挂起**：协程执行完毕后的清理阶段

## Promise_type 详解

Promise_type是协程的核心控制接口，必须实现以下方法：

### 1. get_return_object()

```cpp
task get_return_object() {
    return task{std::coroutine_handle<promise_type>::from_promise(*this)};
}
```

- **调用时机**：协程创建时最先调用
- **作用**：创建返回给调用者的对象
- **注意**：通常需要将promise与返回对象关联

### 2. initial_suspend()

```cpp
std::suspend_always initial_suspend() { return {}; }
// 或
std::suspend_never initial_suspend() { return {}; }
```

- **调用时机**：协程创建后立即调用
- **作用**：决定协程是否立即开始执行
- **返回值**：
  - `std::suspend_always`：协程创建后立即挂起，需要手动恢复
  - `std::suspend_never`：协程创建后立即开始执行

### 3. final_suspend()

```cpp
auto final_suspend() noexcept { return final_awaiter{continuation_}; }
```

- **调用时机**：协程执行完毕后调用
- **作用**：决定协程结束后的行为
- **三种返回值分析**：
  - `std::suspend_never`：协程结束后立即销毁，编译器自动清理
  - `std::suspend_always`：协程结束后始终挂起，等待外部手动销毁
  - **自定义awaiter**：协程结束后根据awaiter的返回值决定是否挂起

> **注意**：三者的`await_resume()`返回值全部被忽略

### 4. return_value() / return_void()

```cpp
void return_value(T&& value) {
    // 存储返回值
    result_ = std::forward<T>(value);
}

// 或
void return_void() {
    // 无返回值的协程
}
```

- **调用时机**：协程执行到`co_return`语句时调用
- **作用**：处理协程的返回值
- **注意**：`return_value`和`return_void`必须实现其中一个，不能同时存在

### 5. unhandled_exception()

```cpp
void unhandled_exception() noexcept {
    exception_ptr_ = std::current_exception();
}
```

- **调用时机**：协程中发生未捕获异常时调用
- **作用**：处理协程中的异常
- **注意**：必须标记为`noexcept`，异常处理中不能再抛出异常

## Awaiter 接口详解

Awaiter是`co_await`表达式的核心，定义了如何暂停和恢复协程。

### await_ready()

```cpp
bool await_ready() noexcept { 
    return true;  // 或 false
}
```

- **返回true**：表示操作已完成，无需挂起，直接调用`await_resume()`
- **返回false**：表示操作未完成，需要挂起协程，调用`await_suspend()`

### await_suspend()

```cpp
// 三种返回值类型
void await_suspend(std::coroutine_handle<> current) noexcept {
    // 挂起当前协程，继续执行调用者
}

bool await_suspend(std::coroutine_handle<> handle) noexcept {
    // 返回true：协程保持挂起状态
    // 返回false：立即恢复协程执行
    return true;
}

std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle) noexcept {
    // 返回另一个协程句柄，立即恢复该协程
    return another_handle;
}
```

- **void返回**：挂起当前协程，返回到调用者
- **bool返回**：true表示保持挂起，false表示立即恢复
- **coroutine_handle返回**：恢复指定的协程

### await_resume()

```cpp
void await_resume() noexcept {
    // 无返回值
}

T await_resume() noexcept {
    // 返回操作结果
    return result;
}
```

- **作用**：提供`co_await`表达式的结果
- **调用时机**：协程恢复执行时调用
- **返回值**：作为`co_await`表达式的值

### co_await 编译器转换过程

```cpp
// 用户写的代码：
auto result = co_await awaiter;

// 编译器生成的伪代码：
{
    auto&& __awaiter = GET_AWAITER(awaiter);
    if (!__awaiter.await_ready()) {
        // 挂起协程
        __coro_frame->__resume_point = N;  // 设置恢复点
        
        // 调用 await_suspend
        auto __suspend_result = __awaiter.await_suspend(
            std::coroutine_handle<>::from_address(__coro_frame)
        );
        
        // 根据 await_suspend 的返回值处理
        HANDLE_SUSPEND_RESULT(__suspend_result);
        
        // 控制权返回给调用者
        return;
    }
    
    // 立即恢复
resume_point_N:
    auto result = __awaiter.await_resume();
}
```

## Coroutine_handle 详解

`std::coroutine_handle`是协程的句柄，提供了对协程的控制能力。

### 主要方法

```cpp
// 检查协程是否已完成
bool done() const noexcept;

// 恢复协程执行
void resume() const noexcept;

// 销毁协程帧
void destroy() const noexcept;

// 获取promise对象的引用
Promise& promise() const noexcept;

// 获取协程帧地址
void* address() const noexcept;

// 从地址创建句柄
static coroutine_handle from_address(void* addr) noexcept;
```

### 生命周期管理

```cpp
// 创建协程
auto task = my_coroutine();
auto handle = task.handle();

// 检查状态
if (!handle.done()) {
    handle.resume();  // 恢复执行
}

// 协程完成后必须手动销毁
if (handle.done()) {
    handle.destroy();
}
```

### 注意事项

1. **destroy()调用**：协程完成后必须调用`destroy()`释放资源
2. **重复销毁**：不能多次调用`destroy()`，会导致未定义行为
3. **空句柄检查**：使用前应检查句柄是否有效
4. **线程安全**：协程句柄本身不是线程安全的

## 实际应用示例

### 简单任务实现

```cpp
#include <coroutine>
#include <iostream>

template<typename T>
class Task {
public:
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        
        void return_value(T value) {
            result_ = std::move(value);
        }
        
        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
        
    private:
        T result_;
        std::exception_ptr exception_;
        friend class Task;
    };
    
    explicit Task(std::coroutine_handle<promise_type> handle) 
        : handle_(handle) {}
    
    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }
    
    T get_result() {
        if (!handle_.done()) {
            handle_.resume();
        }
        
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        
        return std::move(handle_.promise().result_);
    }
    
private:
    std::coroutine_handle<promise_type> handle_;
};

Task<int> calculate() {
    std::cout << "Calculating...\n";
    co_return 42;
}

int main() {
    auto task = calculate();
    int result = task.get_result();
    std::cout << "Result: " << result << std::endl;
    return 0;
}
```

### 异步等待示例

```cpp
struct AsyncAwaiter {
    bool await_ready() noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> handle) {
        // 模拟异步操作
        std::thread([handle]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            handle.resume();
        }).detach();
    }
    
    int await_resume() noexcept { return 100; }
};

Task<int> async_operation() {
    std::cout << "Starting async operation...\n";
    int result = co_await AsyncAwaiter{};
    std::cout << "Async operation completed\n";
    co_return result;
}
```

## 最佳实践

1. **资源管理**：始终确保协程帧被正确销毁
2. **异常处理**：在promise中妥善处理异常
3. **性能考虑**：避免不必要的协程创建和切换
4. **调试技巧**：使用日志跟踪协程状态变化
5. **线程安全**：多线程环境中注意协程状态的同步

## 总结

C++20协程提供了一种强大的异步编程工具，通过理解Promise、Awaiter和Coroutine_handle的工作机制，可以构建高效、可维护的异步代码。正确使用协程可以显著提高程序的性能和可读性。