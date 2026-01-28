
#include "xcoroutine/awaitable_traits.hpp"
#include "xcoroutine/sync_wait.hpp"
#include "xcoroutine/task.hpp"
#include <iostream>
#include <sys/stat.h>
#include <type_traits>

class Data {
public:
  Data() {}
  Data(const Data &) { std::cout << "Data copy constructor" << '\n'; }
  Data &operator=(const Data &) {
    std::cout << "Data copy assignment operator" << '\n';
    return *this;
  }
  Data(Data &&) { std::cout << "Data move constructor" << '\n'; }
  Data &operator=(Data &&) {

    std::cout << "Data move assignment operator" << '\n';
    return *this;
  }
};
Data data;
xcoro::task<Data &> getData() { co_return data; }

int main() {
  std::cout << &data << '\n';
  Data &ret = xcoro::sync_wait(getData());
  std::cout << &ret << '\n';
}
