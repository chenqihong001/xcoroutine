#include "xcoroutine/sync_wait.hpp"
#include "xcoroutine/task.hpp"
#include "xcoroutine/when_all.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>

using namespace xcoro;

namespace {

task<int> make_int(int value) { co_return value; }

task<std::string> make_string(std::string value) {
  co_return std::move(value);
}

task<> increment(int &value) {
  ++value;
  co_return;
}

task<int &> return_ref(int &value) { co_return value; }

task<int> throw_task() {
  throw std::runtime_error("boom");
  co_return 0;
}

task<std::unique_ptr<int>> make_ptr(int value) {
  co_return std::make_unique<int>(value);
}

} // namespace

TEST(WhenAllTest, CollectsValuesFromRvalues) {
  auto result = sync_wait(when_all(make_int(1), make_string("hello")));
  static_assert(
      std::is_same_v<decltype(result), std::tuple<int, std::string>>);
  EXPECT_EQ(std::get<0>(result), 1);
  EXPECT_EQ(std::get<1>(result), "hello");
}

TEST(WhenAllTest, LvalueAwaitableReturnsReferences) {
  auto t1 = make_int(5);
  auto t2 = make_string("world");
  auto all = when_all(t1, t2);
  auto result = sync_wait(all);
  static_assert(
      std::is_same_v<decltype(result), std::tuple<int &, std::string &>>);
  EXPECT_EQ(std::get<0>(result), 5);
  EXPECT_EQ(std::get<1>(result), "world");
}

TEST(WhenAllTest, SupportsVoidTasks) {
  int value = 0;
  sync_wait(when_all(increment(value), increment(value)));
  EXPECT_EQ(value, 2);
}

TEST(WhenAllTest, PropagatesExceptions) {
  EXPECT_THROW(sync_wait(when_all(make_int(1), throw_task())),
               std::runtime_error);
}

TEST(WhenAllTest, SupportsReferenceResults) {
  int value = 7;
  auto result = sync_wait(when_all(return_ref(value)));
  static_assert(std::is_same_v<decltype(result), std::tuple<int &>>);
  std::get<0>(result) = 9;
  EXPECT_EQ(value, 9);
}

TEST(WhenAllTest, SupportsMoveOnlyResults) {
  auto result = sync_wait(when_all(make_ptr(3), make_ptr(4)));
  static_assert(std::is_same_v<decltype(result),
                               std::tuple<std::unique_ptr<int>,
                                          std::unique_ptr<int>>>);
  EXPECT_EQ(*std::get<0>(result), 3);
  EXPECT_EQ(*std::get<1>(result), 4);
}
