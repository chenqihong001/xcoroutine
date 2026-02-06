#include "xcoroutine/awaitable_traits.hpp"
#include <coroutine>
#include <gtest/gtest.h>
#include <type_traits>

using namespace xcoro;

// 测试用的 Awaiter 类型 - 直接实现 Awaiter 接口
struct IntAwaiter {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  int await_resume() const noexcept { return 42; }
};

struct VoidAwaiter {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};

struct BoolSuspendAwaiter {
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<>) const noexcept { return true; }
  double await_resume() const noexcept { return 3.14; }
};

struct HandleSuspendAwaiter {
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> h) const noexcept {
    return h;
  }
  std::string await_resume() const noexcept { return "hello"; }
};

// 测试用的 Awaitable 类型 - 有成员 operator co_await()
struct MemberCoAwaitInt {
  IntAwaiter operator co_await() const noexcept { return {}; }
};

struct MemberCoAwaitVoid {
  VoidAwaiter operator co_await() const noexcept { return {}; }
};

// 测试用的 Awaitable 类型 - 有全局 operator co_await()
struct GlobalCoAwaitInt {};
struct GlobalCoAwaitVoid {};

IntAwaiter operator co_await(GlobalCoAwaitInt) noexcept { return {}; }
VoidAwaiter operator co_await(GlobalCoAwaitVoid) noexcept { return {}; }

// ============ awaiter_t 测试 ============

TEST(AwaiterTraitsTest, AwaiterTypeForDirectAwaiter) {
  // 直接是 Awaiter 的类型，awaiter_t 应该是它自己
  static_assert(std::is_same_v<awaiter_t<IntAwaiter>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<VoidAwaiter>, VoidAwaiter>);
  static_assert(
      std::is_same_v<awaiter_t<BoolSuspendAwaiter>, BoolSuspendAwaiter>);
  static_assert(
      std::is_same_v<awaiter_t<HandleSuspendAwaiter>, HandleSuspendAwaiter>);
}

TEST(AwaiterTraitsTest, AwaiterTypeForMemberCoAwait) {
  // 有成员 operator co_await() 的类型，awaiter_t 应该是它返回的 awaiter 类型
  static_assert(std::is_same_v<awaiter_t<MemberCoAwaitInt>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<MemberCoAwaitVoid>, VoidAwaiter>);
}

TEST(AwaiterTraitsTest, AwaiterTypeForGlobalCoAwait) {
  // 有全局 operator co_await() 的类型，awaiter_t 应该是它返回的 awaiter 类型
  static_assert(std::is_same_v<awaiter_t<GlobalCoAwaitInt>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<GlobalCoAwaitVoid>, VoidAwaiter>);
}

TEST(AwaiterTraitsTest, AwaiterTypeWithReferences) {
  // 测试左值引用和右值引用的情况
  static_assert(std::is_same_v<awaiter_t<IntAwaiter &>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<IntAwaiter &&>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<const IntAwaiter &>, IntAwaiter>);

  static_assert(std::is_same_v<awaiter_t<MemberCoAwaitInt &>, IntAwaiter>);
  static_assert(std::is_same_v<awaiter_t<MemberCoAwaitInt &&>, IntAwaiter>);
}

// ============ awaiter_result_t 测试 ============

TEST(AwaiterResultTest, ResultTypeForDirectAwaiter) {
  // 测试直接 Awaiter 的 await_resume() 返回类型
  static_assert(std::is_same_v<awaiter_result_t<IntAwaiter>, int>);
  static_assert(std::is_same_v<awaiter_result_t<VoidAwaiter>, void>);
  static_assert(std::is_same_v<awaiter_result_t<BoolSuspendAwaiter>, double>);
  static_assert(
      std::is_same_v<awaiter_result_t<HandleSuspendAwaiter>, std::string>);
}

TEST(AwaiterResultTest, ResultTypeForMemberCoAwait) {
  // 测试有成员 operator co_await() 的类型的结果类型
  static_assert(std::is_same_v<awaiter_result_t<MemberCoAwaitInt>, int>);
  static_assert(std::is_same_v<awaiter_result_t<MemberCoAwaitVoid>, void>);
}

TEST(AwaiterResultTest, ResultTypeForGlobalCoAwait) {
  // 测试有全局 operator co_await() 的类型的结果类型
  static_assert(std::is_same_v<awaiter_result_t<GlobalCoAwaitInt>, int>);
  static_assert(std::is_same_v<awaiter_result_t<GlobalCoAwaitVoid>, void>);
}

TEST(AwaiterResultTest, ResultTypeWithReferences) {
  // 测试引用类型的情况
  static_assert(std::is_same_v<awaiter_result_t<IntAwaiter &>, int>);
  static_assert(std::is_same_v<awaiter_result_t<IntAwaiter &&>, int>);
  static_assert(std::is_same_v<awaiter_result_t<const IntAwaiter &>, int>);

  static_assert(std::is_same_v<awaiter_result_t<MemberCoAwaitInt &>, int>);
  static_assert(std::is_same_v<awaiter_result_t<MemberCoAwaitInt &&>, int>);
}

// ============ 集成测试 ============

TEST(AwaiterTraitsTest, CompleteWorkflow) {
  // 测试完整的工作流程：从 Awaitable 获取 Awaiter，再获取结果类型

  // 直接 Awaiter
  using DirectAwaiter = IntAwaiter;
  using DirectAwaiterType = awaiter_t<DirectAwaiter>;
  using DirectResultType = awaiter_result_t<DirectAwaiter>;
  static_assert(std::is_same_v<DirectAwaiterType, IntAwaiter>);
  static_assert(std::is_same_v<DirectResultType, int>);

  // 成员 operator co_await()
  using MemberAwaitable = MemberCoAwaitInt;
  using MemberAwaiterType = awaiter_t<MemberAwaitable>;
  using MemberResultType = awaiter_result_t<MemberAwaitable>;
  static_assert(std::is_same_v<MemberAwaiterType, IntAwaiter>);
  static_assert(std::is_same_v<MemberResultType, int>);

  // 全局 operator co_await()
  using GlobalAwaitable = GlobalCoAwaitInt;
  using GlobalAwaiterType = awaiter_t<GlobalAwaitable>;
  using GlobalResultType = awaiter_result_t<GlobalAwaitable>;
  static_assert(std::is_same_v<GlobalAwaiterType, IntAwaiter>);
  static_assert(std::is_same_v<GlobalResultType, int>);

  SUCCEED();
}
