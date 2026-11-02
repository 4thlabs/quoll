//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <quoll/async/await_context.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// Counts the allocations of each thread, to check that operations on an any_stream make none. The replacement is
// global to the test program, the count is per thread: threads of other tests do not show in it. The standard's
// array and nothrow forms call this one; over-aligned allocations are not counted.
namespace {

constinit thread_local std::size_t allocations = 0;

} // namespace

void *operator new(std::size_t size) {
  ++allocations;

  if (auto *const memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }

  throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

namespace io = quoll::io;

using quoll::async::task;

using quoll::test::overloaded;
using quoll::test::run;

std::span<std::byte const> bytes(std::string_view text) { return std::as_bytes(std::span{text}); }

// A stream that never allocates: reads fill the buffer with `fill` after a round trip through the executor, writes
// accept everything and resume the writer at once. `Padding` makes it too big to be held in place, and so does a
// move that may throw. Its awaitables cannot move, which any_stream supports: it builds them where they stay.
template <std::size_t Padding, bool NothrowMove = true>
class fixed_stream {
public:
  static constexpr std::byte fill{0x2a};

  class read_awaitable {
  public:
    explicit read_awaitable(std::span<std::byte> buffer) noexcept : buffer_(buffer) {}

    read_awaitable(read_awaitable const &) = delete;
    read_awaitable(read_awaitable &&) = delete;
    read_awaitable &operator=(read_awaitable const &) = delete;
    read_awaitable &operator=(read_awaitable &&) = delete;
    ~read_awaitable() = default;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    // Declines to suspend for an empty buffer.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, quoll::async::await_context const &context) {
      if (buffer_.empty()) {
        return false;
      }

      item_.handle = handle;
      context.executor.post(item_);
      return true;
    }

    [[nodiscard]] io::io_result await_resume() noexcept {
      std::ranges::fill(buffer_, fill);
      return {{}, buffer_.size()};
    }

  private:
    std::span<std::byte> buffer_;
    quoll::async::work_item item_;
  };

  class write_awaitable {
  public:
    explicit write_awaitable(std::span<std::byte const> data) noexcept : data_(data) {}

    write_awaitable(write_awaitable const &) = delete;
    write_awaitable(write_awaitable &&) = delete;
    write_awaitable &operator=(write_awaitable const &) = delete;
    write_awaitable &operator=(write_awaitable &&) = delete;
    ~write_awaitable() = default;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    // Transfers straight back to the writer.
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                                        quoll::async::await_context const &) const noexcept {
      return handle;
    }

    [[nodiscard]] io::io_result await_resume() const noexcept { return {{}, data_.size()}; }

  private:
    std::span<std::byte const> data_;
  };

  fixed_stream() = default;
  fixed_stream(fixed_stream const &) = delete;
  fixed_stream &operator=(fixed_stream const &) = delete;
  fixed_stream &operator=(fixed_stream &&) = delete;
  ~fixed_stream() = default;

  fixed_stream(fixed_stream &&) noexcept(NothrowMove) {}

  [[nodiscard]] read_awaitable read_some(std::span<std::byte> buffer) noexcept { return read_awaitable{buffer}; }

  [[nodiscard]] write_awaitable write_some(std::span<std::byte const> data) noexcept { return write_awaitable{data}; }

private:
  [[maybe_unused]] std::array<std::byte, Padding> padding_{};
};

using small_stream = fixed_stream<0>;
using big_stream = fixed_stream<256>;
using throwing_move_stream = fixed_stream<0, false>;

static_assert(io::ByteStream<small_stream>);
static_assert(io::ByteStream<big_stream>);
static_assert(io::ByteStream<throwing_move_stream>);

// What a read brought, as text, with its error.
struct received {
  std::string text;
  std::error_code error;
};

task<received> read_once(io::any_stream &stream) {
  std::array<std::byte, 64> storage{};
  auto const result = co_await stream.read_some(storage);

  co_return received{std::string{reinterpret_cast<char const *>(storage.data()), result.transferred}, result.error};
}

task<io::io_result> write_all(io::any_stream &stream, std::string_view text) {
  co_return co_await io::write(stream, bytes(text));
}

// Reads and writes `rounds` times, and returns the allocations they made.
task<std::size_t> read_and_write(io::any_stream &stream, int rounds) {
  std::array<std::byte, 16> buffer{};
  auto const before = allocations;

  for (int round = 0; round < rounds; ++round) {
    auto const read = co_await stream.read_some(buffer);
    auto const written = co_await stream.write_some(buffer);
    auto const empty = co_await stream.read_some({});

    if (read.transferred != buffer.size() || written.transferred != buffer.size() || empty.transferred != 0 ||
        buffer.back() != small_stream::fill) {
      throw std::logic_error{"unexpected transfer"};
    }
  }

  co_return allocations - before;
}

} // namespace

TEST_CASE("any_stream: reads and writes through the wrapped stream", "[io][any_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  io::any_stream client{std::move(left)};
  io::any_stream server{std::move(right)};

  REQUIRE_FALSE(run(write_all(client, "hello"))->error);

  auto const read = run(read_once(server));

  REQUIRE(read);
  REQUIRE(read->text == "hello");
}

TEST_CASE("any_stream: a stop request reaches the wrapped read", "[io][any_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  io::any_stream stream{std::move(left)};
  quoll::async::run_loop loop;
  std::stop_source source;
  std::optional<received> read;

  co_spawn(loop.get_executor(), read_once(stream),
           overloaded{[&](received value) { read = std::move(value); }, [](std::exception_ptr) {}}, source.get_token());
  loop.run();

  REQUIRE_FALSE(read);

  source.request_stop();
  loop.run();

  REQUIRE(read);
  REQUIRE(read->error == std::errc::operation_canceled);
}

TEST_CASE("any_stream: reading and writing do not allocate", "[io][any_stream]") {
  SECTION("a stream held in place") {
    io::any_stream stream{small_stream{}};

    REQUIRE(run(read_and_write(stream, 100)) == 0U);
  }

  SECTION("a stream on the heap") {
    io::any_stream stream{big_stream{}};

    REQUIRE(run(read_and_write(stream, 100)) == 0U);
  }
}

TEST_CASE("any_stream: a small stream is held in place, a big one on the heap", "[io][any_stream]") {
  auto const before = allocations;
  io::any_stream small{small_stream{}};

  REQUIRE(allocations == before);

  io::any_stream big{big_stream{}};

  REQUIRE(allocations == before + 1);

  auto const *const small_address = small.target<small_stream>();
  auto const *const big_address = big.target<big_stream>();
  auto moved_small = std::move(small);
  auto moved_big = std::move(big);

  // Moving relocates a stream held in place, and hands the heap block over.
  REQUIRE(moved_small.target<small_stream>() != nullptr);
  REQUIRE(moved_small.target<small_stream>() != small_address);
  REQUIRE(moved_big.target<big_stream>() == big_address);
  REQUIRE(small.target<small_stream>() == nullptr);
  REQUIRE(big.target<big_stream>() == nullptr);
  REQUIRE(run(read_and_write(moved_small, 1)) == 0U);
  REQUIRE(run(read_and_write(moved_big, 1)) == 0U);
}

TEST_CASE("any_stream: a stream whose move may throw is put on the heap", "[io][any_stream]") {
  auto const before = allocations;
  io::any_stream stream{throwing_move_stream{}};

  REQUIRE(allocations == before + 1);

  auto const *const address = stream.target<throwing_move_stream>();
  auto moved = std::move(stream);

  REQUIRE(moved.target<throwing_move_stream>() == address);
  REQUIRE(run(read_and_write(moved, 1)) == 0U);
}

TEST_CASE("any_stream: move assignment replaces the wrapped stream", "[io][any_stream]") {
  io::any_stream stream{small_stream{}};
  io::any_stream big{big_stream{}};
  auto const *const big_address = big.target<big_stream>();

  // Over a stream held in place, with one from the heap.
  stream = std::move(big);

  REQUIRE(stream.target<small_stream>() == nullptr);
  REQUIRE(stream.target<big_stream>() == big_address);
  REQUIRE(run(read_and_write(stream, 1)) == 0U);

  // Over a stream from the heap, with one held in place.
  stream = io::any_stream{small_stream{}};

  REQUIRE(stream.target<big_stream>() == nullptr);
  REQUIRE(stream.target<small_stream>() != nullptr);
  REQUIRE(run(read_and_write(stream, 1)) == 0U);
}

TEST_CASE("any_stream: target() recovers the wrapped stream by its exact type", "[io][any_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  io::any_stream stream{std::move(left)};

  REQUIRE(stream.target<io::memory_stream>() != nullptr);
  REQUIRE(std::as_const(stream).target<io::memory_stream>() != nullptr);
  REQUIRE(stream.target<small_stream>() == nullptr);

  // Closing the wrapped stream reaches the peer.
  stream.target<io::memory_stream>()->close();

  io::any_stream peer{std::move(right)};
  auto const read = run(read_once(peer));

  REQUIRE(read);
  REQUIRE(read->error == io::error::end_of_stream);
}
