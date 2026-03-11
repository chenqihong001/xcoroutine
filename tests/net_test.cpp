#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <future>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xcoro/cancellation_source.hpp"
#include "xcoro/sync_wait.hpp"

using namespace xcoro;
using namespace xcoro::net;

using xcoro_socket = xcoro::net::socket;

namespace {

class scoped_fd {
 public:
  scoped_fd() = default;
  explicit scoped_fd(int fd) : fd_(fd) {}
  scoped_fd(const scoped_fd&) = delete;
  scoped_fd& operator=(const scoped_fd&) = delete;
  scoped_fd(scoped_fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  scoped_fd& operator=(scoped_fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~scoped_fd() { reset(); }

  void reset(int new_fd = -1) {
    if (fd_ != -1) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

  int get() const { return fd_; }

 private:
  int fd_{-1};
};

}  // namespace

TEST(NetTest, OpenTcpSocketHasNonBlockingAndCloexec) {
  io_context ctx;
  xcoro_socket sock;
  try {
    sock = xcoro_socket::open_tcp(ctx);
  } catch (const std::system_error& e) {
    if (e.code().value() == EPERM || e.code().value() == EACCES ||
        e.code().value() == EAFNOSUPPORT) {
      GTEST_SKIP() << "tcp socket creation is not permitted in this environment";
    }
    throw;
  }

  int flags = ::fcntl(sock.native_handle(), F_GETFL, 0);
  ASSERT_NE(flags, -1);
  EXPECT_NE(flags & O_NONBLOCK, 0);

  int fd_flags = ::fcntl(sock.native_handle(), F_GETFD, 0);
  ASSERT_NE(fd_flags, -1);
  EXPECT_NE(fd_flags & FD_CLOEXEC, 0);
}

TEST(NetTest, SocketMoveAssignmentClosesOldFd) {
  io_context ctx;
  xcoro_socket first;
  xcoro_socket second;
  try {
    first = xcoro_socket::open_tcp(ctx);
    second = xcoro_socket::open_tcp(ctx);
  } catch (const std::system_error& e) {
    if (e.code().value() == EPERM || e.code().value() == EACCES ||
        e.code().value() == EAFNOSUPPORT) {
      GTEST_SKIP() << "tcp socket creation is not permitted in this environment";
    }
    throw;
  }

  const int old_fd = first.native_handle();
  const int second_fd = second.native_handle();

  first = std::move(second);

  EXPECT_EQ(first.native_handle(), second_fd);
  errno = 0;
  EXPECT_EQ(::fcntl(old_fd, F_GETFD, 0), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(NetTest, AsyncReadExactAndWriteAllOverSocketPair) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  scoped_fd left{fds[0]};
  scoped_fd right{fds[1]};

  io_context ctx;
  ctx.run();

  constexpr char message[] = "hello-io-context";
  std::array<char, sizeof(message)> buffer{};

  const auto written = sync_wait(ctx.async_write_all(left.get(), message, sizeof(message)));
  const auto read = sync_wait(ctx.async_read_exact(right.get(), buffer.data(), buffer.size()));

  EXPECT_EQ(written, sizeof(message));
  EXPECT_EQ(read, sizeof(message));
  EXPECT_EQ(std::memcmp(buffer.data(), message, sizeof(message)), 0);

  ctx.stop();
}

TEST(NetTest, AsyncReadSomeCanBeCancelled) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  scoped_fd left{fds[0]};
  scoped_fd right{fds[1]};

  io_context ctx;
  cancellation_source source;
  std::array<char, 8> buffer{};
  ctx.run();

  auto reader = std::async(std::launch::async, [&] {
    try {
      (void)sync_wait(ctx.async_read_some(left.get(), buffer.data(), buffer.size(), source.token()));
      return false;
    } catch (const operation_cancelled&) {
      return true;
    } catch (...) {
      return false;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  source.request_cancellation();
  EXPECT_TRUE(reader.get());

  ctx.stop();
}

TEST(NetTest, AsyncWriteAllFallsBackToWriteForPipeFd) {
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC), 0);
  scoped_fd read_end{pipe_fds[0]};
  scoped_fd write_end{pipe_fds[1]};

  io_context ctx;

  constexpr char message[] = "pipe-write";
  const size_t written = sync_wait(ctx.async_write_all(write_end.get(), message, sizeof(message)));
  EXPECT_EQ(written, sizeof(message));

  std::array<char, sizeof(message)> out{};
  const ssize_t n = ::read(read_end.get(), out.data(), out.size());
  ASSERT_EQ(n, static_cast<ssize_t>(sizeof(message)));
  EXPECT_EQ(std::memcmp(out.data(), message, sizeof(message)), 0);
}

TEST(NetTest, AsyncWriteAllToClosedPeerReturnsErrorNotSignal) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  scoped_fd writer_fd{fds[0]};
  scoped_fd reader_fd{fds[1]};

  reader_fd.reset();
  io_context ctx;

  constexpr char message[] = "will-fail";
  bool got_expected_error = false;
  try {
    (void)sync_wait(ctx.async_write_all(writer_fd.get(), message, sizeof(message)));
  } catch (const std::system_error& e) {
    got_expected_error = (e.code().value() == EPIPE || e.code().value() == ECONNRESET);
  }
  EXPECT_TRUE(got_expected_error);
}
