#include "xcoroutine/task.hpp"
#include <iostream>

class Data {
public:
  Data() {}
  Data(const Data &) { std::cout << "Data(const Data &)\n"; }
  Data(Data &&) { std::cout << "Data(Data &&)\n"; }
};

xcoro::task<Data> getName() { co_return Data{}; }
xcoro::task<int> compute(int x, int y) {
  Data data = co_await getName();
  // 有移动构造调用移动构造
  std::cout << "compute(int x, int y) move constructor\n";
  // std::cout << str << '\n';
  co_return x + y;
}

Data &&getData() {
  Data data;
  return std::move(data);
}

int main() { Data &&data = getData(); }