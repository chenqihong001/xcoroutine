#include <coroutine>
#include <iostream>

template <class T> class Task;

template <class T> struct Promise {
  Task<T> get_return_object() {
    return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this));
  }
  std::suspend_always initial_suspend() { return {}; }
  std::suspend_always final_suspend() noexcept { return {}; }
  void return_value(T value) { this->value = std::move(value); }
  void unhandled_exception() { std::terminate(); }
  T value;
};

template <class T> class Task {
public:
  using promise_type = Promise<T>;

  Task(std::coroutine_handle<Promise<T>> h) : coro(h) {}

  ~Task() {
    if (coro) {
      coro.destroy();
    }
  }

  void resume() {
    if (coro && !coro.done()) {
      coro.resume();
    }
  }

  T get_result() {
    if (coro && coro.done()) {
      return coro.promise().value;
    }
    throw std::runtime_error("Coroutine not completed");
  }

  // Helper method to check handle state
  void check_handle_state(const std::string &label) const {
    std::cout << "[" << label << "] ";
    if (coro) {
      std::cout << "Handle valid, ";
      if (coro.done()) {
        std::cout << "Coroutine completed";
      } else {
        std::cout << "Coroutine not completed";
      }
    } else {
      std::cout << "Handle invalid";
    }
    std::cout << std::endl;
  }

  std::coroutine_handle<Promise<T>> coro;
};

Task<int> func() {
  std::cout << "Coroutine started..." << std::endl;
  co_return 42;
}

int main() {
  std::cout << "=== Coroutine Handle State Demo ===" << std::endl;

  // 1. Create coroutine
  auto task = func();
  task.check_handle_state("After creation");

  // 2. Resume coroutine execution
  task.resume();
  task.check_handle_state("After resume");

  // 3. Get result
  try {
    int result = task.get_result();
    std::cout << "Coroutine result: " << result << std::endl;
    task.check_handle_state("After getting result");
  } catch (const std::exception &e) {
    std::cout << "Error: " << e.what() << std::endl;
  }

  // 4. Manually destroy coroutine
  if (task.coro) {
    std::cout << "Manually destroying coroutine..." << std::endl;
    task.coro.destroy();
    task.check_handle_state("After destruction");
  }

  // 5. Try to access destroyed coroutine
  try {
    task.resume(); // This will not execute any operation
    std::cout << "Attempting to resume destroyed coroutine" << std::endl;
  } catch (...) {
    std::cout << "Exception occurred when accessing destroyed coroutine"
              << std::endl;
  }

  return 0;
}