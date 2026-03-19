#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>
namespace xcoro::net {
struct mutable_buffer {
  std::span<std::byte> bytes;
};

struct const_buffer {
  std::span<const std::byte> bytes;
};

class byte_buffer {
 public:
  explicit byte_buffer(size_t initial = 4096)
      : buf_(std::max<size_t>(initial, 1)), read_(0), size_(0) {}

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return buf_.size(); }
  size_t writable() const noexcept { return capacity() - size_; }

  std::span<const std::byte> data() const noexcept {
    return readable_regions()[0];
  }

  std::array<std::span<const std::byte>, 2> readable_regions() const noexcept {
    if (size_ == 0) {
      return {std::span<const std::byte>{}, std::span<const std::byte>{}};
    }
    const size_t first_len = std::min(size_, capacity() - read_);
    const size_t second_len = size_ - first_len;
    return {std::span<const std::byte>(buf_.data() + read_, first_len),
            std::span<const std::byte>(buf_.data(), second_len)};
  }

  std::array<std::span<std::byte>, 2> prepare_regions(
      size_t n = std::numeric_limits<size_t>::max()) {
    if (n == 0) {
      return {std::span<std::byte>{}, std::span<std::byte>{}};
    }

    if (n == std::numeric_limits<size_t>::max()) {
      n = writable();
    } else {
      ensure_writable(n);
    }

    const size_t tail = write_index();
    if (tail < read_) {
      const size_t len = std::min(n, read_ - tail);
      return {std::span<std::byte>(buf_.data() + tail, len), std::span<std::byte>{}};
    }

    const size_t first_len = std::min(n, capacity() - tail);
    const size_t second_len = std::min(n - first_len, read_);
    return {std::span<std::byte>(buf_.data() + tail, first_len),
            std::span<std::byte>(buf_.data(), second_len)};
  }

  std::span<std::byte> prepare(size_t n) {
    if (n == 0) {
      return {};
    }
    ensure_writable(n);
    if (writable_contiguous() < n) {
      linearize();
    }
    assert(writable_contiguous() >= n);
    return std::span<std::byte>(buf_.data() + write_index(), n);
  }
  void commit(std::size_t n) noexcept {
    assert(n <= writable());
    size_ += n;
  }

  void consume(std::size_t n) noexcept {
    assert(n <= size());
    if (n == 0) {
      return;
    }
    read_ = (read_ + n) % capacity();
    size_ -= n;
    if (size_ == 0) {
      clear();
    }
  }
  void clear() noexcept {
    read_ = 0;
    size_ = 0;
  }

 private:
  size_t write_index() const noexcept { return (read_ + size_) % capacity(); }

  size_t writable_contiguous() const noexcept {
    if (size_ == capacity()) {
      return 0;
    }
    const size_t tail = write_index();
    if (tail < read_) {
      return read_ - tail;
    }
    return capacity() - tail;
  }

  void ensure_writable(std::size_t n) {
    if (writable() >= n) {
      return;
    }
    size_t new_capacity = capacity();
    const size_t need = size_ + n;
    while (new_capacity < need) {
      new_capacity *= 2;
    }
    grow_and_linearize(new_capacity);
  }

  void linearize() {
    if (size_ == 0) {
      read_ = 0;
      return;
    }
    if (read_ + size_ <= capacity()) {
      if (read_ != 0) {
        std::memmove(buf_.data(), buf_.data() + read_, size_);
        read_ = 0;
      }
      return;
    }

    std::vector<std::byte> tmp(size_);
    const auto regions = readable_regions();
    std::memcpy(tmp.data(), regions[0].data(), regions[0].size());
    std::memcpy(tmp.data() + regions[0].size(), regions[1].data(), regions[1].size());
    std::memcpy(buf_.data(), tmp.data(), size_);
    read_ = 0;
  }

  void grow_and_linearize(size_t new_capacity) {
    std::vector<std::byte> new_buf(new_capacity);
    const auto regions = readable_regions();
    std::memcpy(new_buf.data(), regions[0].data(), regions[0].size());
    std::memcpy(new_buf.data() + regions[0].size(), regions[1].data(), regions[1].size());
    buf_.swap(new_buf);
    read_ = 0;
  }

  std::vector<std::byte> buf_;
  size_t read_;
  size_t size_;
};

}  // namespace xcoro::net
