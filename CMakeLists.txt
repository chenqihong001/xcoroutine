cmake_minimum_required(VERSION 3.10)
project(XCoroutine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
find_package(Threads REQUIRED)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)


enable_testing()

set(TEST_FILES
 "tests/generator_test.cpp"
 "tests/cancellation_test.cpp"
 "tests/net_test.cpp"
 "tests/semaphore_test.cpp"
 "tests/mutex_test.cpp"
 "tests/condition_variable_test.cpp"
 "tests/latch_test.cpp"
 "tests/event_test.cpp"
 "tests/when_any_test.cpp"
 "tests/when_all_test.cpp"
)
add_executable(xcoro_test ${TEST_FILES})
target_link_libraries(xcoro_test GTest::gtest_main Threads::Threads)
target_include_directories(xcoro_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

include(GoogleTest)
gtest_discover_tests(xcoro_test)
