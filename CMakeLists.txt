cmake_minimum_required(VERSION 3.10)
project(XCoroutine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(xcoro main.cpp)
target_include_directories(xcoro PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()

set(TEST_FILES
  "tests/task_test.cpp"
  "tests/awaitable_traits_test.cpp"
)
add_executable(xcoro_test ${TEST_FILES})
target_link_libraries(xcoro_test GTest::gtest_main)
target_include_directories(xcoro_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

include(GoogleTest)
gtest_discover_tests(xcoro_test)
