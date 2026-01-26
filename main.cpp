#include <coroutine>
#include <exception>
#include <iostream>
class task {
public:
  struct final_awaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
      if (continue_handle_) {
        return continue_handle_;
      }
      return std::noop_coroutine(); // 没有继续的协程 直接返回空句柄，返回主程序
    }
    void await_resume() noexcept {} // 返回值被忽略

    std::coroutine_handle<> continue_handle_;
  };

  struct promise_type {
    task get_return_object() {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }
    template <typename T> void return_value(T &&value) {
      std::cout << "return_value: " << value << std::endl;
    }
    void unhandled_exception() noexcept {
      auto exception = std::current_exception();
      std::rethrow_exception(exception);
    }
    auto final_suspend() noexcept { return final_awaiter{continuation_}; }

    std::coroutine_handle<> continuation_;
  };

  struct awaiter {
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> parent) noexcept {
      handle_.promise().continuation_ = parent;
      return handle_;
    }
    int await_resume() noexcept { return 42; }
    explicit awaiter(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}
    std::coroutine_handle<promise_type> handle_;
  };

  awaiter operator co_await() { return awaiter{handle_}; }

  explicit task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  ~task() {
    if (handle_ && handle_.done()) {
      handle_.destroy();
    }
  }
  void start() {
    if (handle_) {
      handle_.resume();
    }
  }
  std::coroutine_handle<> handle() { return handle_; }

private:
  std::coroutine_handle<promise_type> handle_;
};
// final_suspend 必须noexcept 不允许异常破坏协程清理
// unhandled_exception 必须noexcept 异常处理中不能再抛出异常

struct Awaiter {
  bool await_ready() noexcept {
    std::cout << "await_ready" << std::endl;
    return false;
  }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> handle) noexcept {
    std::cout << "handle:" << handle.address() << std::endl;
    std::cout << "await_suspend" << std::endl;
    return handle;
  }
  void await_resume() noexcept { std::cout << "await_resume" << std::endl; }
};

task func2() {
  std::cout << "func2" << std::endl;
  co_await Awaiter{};
  std::cout << "func2 after await" << std::endl;
  co_return "csaniucsa";
}

task func() {
  std::cout << "func" << std::endl;
  co_await func2();
  std::cout << "func after await" << std::endl;
  co_return "csaniucsa";
}

int main() {
  auto coro = func();

  coro.start();

  return 0;
}