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
#include <quoll/async/when_all.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io.hpp>
#include <quoll/test/allocations.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

// Counts the allocations of each thread, to check that operations on an any_stream make none, and fails one when a test
// asks: see quoll::test::allocations.
using quoll::test::allocations;

void *operator new(std::size_t size) {
  ++allocations;

  if (quoll::test::allocations_before_failing == 0) {
    quoll::test::allocations_before_failing = -1;
    throw std::bad_alloc{};
  }

  if (quoll::test::allocations_before_failing > 0) {
    --quoll::test::allocations_before_failing;
  }

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

// Returns a reference to an awaitable it keeps: a ByteStream, but not one any_stream can build awaitables for.
struct reference_stream {
  struct awaitable {
    [[nodiscard]] bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>, quoll::async::await_context const &) const noexcept {}

    [[nodiscard]] io::io_result await_resume() const noexcept { return {}; }
  };

  [[nodiscard]] awaitable &read_some(std::span<std::byte>) noexcept { return operation; }

  [[nodiscard]] awaitable &write_some(std::span<std::byte const>) noexcept { return operation; }

  awaitable operation;
};

static_assert(io::ByteStream<reference_stream>);
static_assert(!std::constructible_from<io::any_stream, reference_stream>);

// The memory_stream tests below cover the third form of await_suspend: theirs returns void.
static_assert(std::is_void_v<decltype(std::declval<io::memory_stream::read_awaitable &>().await_suspend(
                  std::coroutine_handle<>{}, std::declval<quoll::async::await_context const &>()))>);

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
  // Each count brackets one construction only: Catch2 may allocate while it checks an assertion.
  auto const before_small = allocations;
  io::any_stream small{small_stream{}};
  auto const small_allocations = allocations - before_small;
  auto const before_big = allocations;
  io::any_stream big{big_stream{}};
  auto const big_allocations = allocations - before_big;

  REQUIRE(small_allocations == 0U);
  REQUIRE(big_allocations == 1U);

  auto const *const small_address = small.target<small_stream>();
  auto const *const big_address = big.target<big_stream>();
  auto moved_small = std::move(small);
  auto moved_big = std::move(big);

  // Moving relocates a stream held in place, and hands the heap block over.
  REQUIRE(moved_small.target<small_stream>() != nullptr);
  REQUIRE(moved_small.target<small_stream>() != small_address);
  REQUIRE(moved_big.target<big_stream>() == big_address);
  // What a moved-from stream holds is the point here.
  REQUIRE(small.target<small_stream>() == nullptr); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(big.target<big_stream>() == nullptr);     // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(run(read_and_write(moved_small, 1)) == 0U);
  REQUIRE(run(read_and_write(moved_big, 1)) == 0U);
}

TEST_CASE("any_stream: a stream whose move may throw is put on the heap", "[io][any_stream]") {
  auto const before = allocations;
  io::any_stream stream{throwing_move_stream{}};
  auto const made = allocations - before;

  REQUIRE(made == 1U);

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

TEST_CASE("any_stream: shutdown_send goes to a stream that can, and fails on one that cannot", "[io][any_stream]") {
  auto [client, server] = quoll::io::make_memory_stream_pair();
  quoll::io::any_stream closing{std::move(server)};
  quoll::io::any_stream plain{fixed_stream<8>{}};

  auto const steps = [&]() -> quoll::async::task<std::error_code> {
    std::array<char, 4> chunk{};

    REQUIRE_FALSE((co_await closing.shutdown_send()).error);
    REQUIRE((co_await client.read_some(std::as_writable_bytes(std::span{chunk}))).error ==
            quoll::io::error::end_of_stream);
    co_return (co_await plain.shutdown_send()).error;
  };

  static_assert(!quoll::io::HalfClosableStream<fixed_stream<8>>);
  REQUIRE(run(steps()) == std::errc::operation_not_supported);
}

TEST_CASE("any_stream: poll_read goes to a stream that can, and is unknown on one that cannot", "[io][any_stream]") {
  auto [client, server] = quoll::io::make_memory_stream_pair();
  quoll::io::any_stream polled{std::move(server)};
  quoll::io::any_stream plain{fixed_stream<8>{}};

  static_assert(!quoll::io::PollableStream<fixed_stream<8>>);
  REQUIRE(polled.pollable());
  REQUIRE_FALSE(plain.pollable());
  REQUIRE(polled.poll_read() == quoll::io::read_readiness::would_wait);
  REQUIRE(plain.poll_read() == quoll::io::read_readiness::unknown);

  client.close();

  REQUIRE(polled.poll_read() == quoll::io::read_readiness::ready);

  // A read pending: nothing to ask either, as the stream is the read's.
  quoll::async::run_loop loop;
  std::array<std::byte, 4> chunk{};
  auto [peer_side, other] = quoll::io::make_memory_stream_pair();
  quoll::io::any_stream reading{std::move(peer_side)};
  std::optional<quoll::io::read_readiness> during;

  auto const read = [&]() -> quoll::async::task<std::size_t> {
    co_return (co_await reading.read_some(chunk)).transferred;
  };
  auto const look = [&]() -> quoll::async::task<std::size_t> {
    during = reading.poll_read();
    co_return (co_await other.write_some(std::as_bytes(std::span{"x", 1}))).transferred;
  };

  REQUIRE(quoll::test::run(loop, [&]() -> quoll::async::task<std::size_t> {
            auto const [got, sent] = co_await quoll::async::when_all(read(), look());

            co_return got + sent;
          }()) == 2);
  REQUIRE(during == quoll::io::read_readiness::unknown);

  // Moved from: nothing to ask.
  auto const moved = std::move(polled);

  REQUIRE(polled.poll_read() == quoll::io::read_readiness::unknown); // NOLINT(bugprone-use-after-move)
  REQUIRE_FALSE(polled.pollable());                                  // NOLINT(bugprone-use-after-move)
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

namespace {

// A stream whose operations are tasks, as those of a socket with TLS are, and that counts the frames alive: an
// awaitable destroyed twice, or not at all, shows here, and under ASan.
struct task_stream {
  std::shared_ptr<int> frames = std::make_shared<int>(0);

  struct frame_count {
    std::shared_ptr<int> count;

    explicit frame_count(std::shared_ptr<int> counted) : count(std::move(counted)) { ++*count; }

    frame_count(frame_count const &) = delete;
    frame_count &operator=(frame_count const &) = delete;

    ~frame_count() { --*count; }
  };

  [[nodiscard]] static task<io::io_result> reading(std::shared_ptr<int> counted, std::size_t size) {
    frame_count const alive{std::move(counted)};

    co_return io::io_result{{}, size};
  }

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) { return reading(frames, buffer.size()); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) { return reading(frames, data.size()); }
};

static_assert(io::ByteStream<task_stream>);

// Over-aligned, so that it cannot be held in place.
struct alignas(64) aligned_stream {
  [[nodiscard]] static task<io::io_result> done() { co_return io::io_result{{}, 1}; }

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { return done(); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) { return done(); }
};

// Counts how many of it are gone.
struct counted_stream {
  std::shared_ptr<int> gone;

  counted_stream(counted_stream const &) = delete;
  counted_stream &operator=(counted_stream const &) = delete;

  counted_stream(counted_stream &&other) noexcept : gone(std::move(other.gone)) {}

  counted_stream &operator=(counted_stream &&) = delete;

  explicit counted_stream(std::shared_ptr<int> counter) : gone(std::move(counter)) {}

  ~counted_stream() {
    if (gone) {
      ++*gone;
    }
  }

  [[nodiscard]] static task<io::io_result> done() { co_return io::io_result{{}, 1}; }

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { return done(); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) { return done(); }
};

} // namespace

TEST_CASE("any_stream: an operation that has completed leaves room for the next, in one expression",
          "[io][any_stream]") {
  auto const frames = std::make_shared<int>(0);
  std::array<std::byte, 4> buffer{};

  auto const reading = [&]() -> task<std::size_t> {
    io::any_stream stream{task_stream{frames}};
    auto const read_twice =
        (co_await stream.read_some(buffer)).transferred + (co_await stream.read_some(buffer)).transferred;
    auto const write_twice =
        (co_await stream.write_some(buffer)).transferred + (co_await stream.write_some(buffer)).transferred;

    co_return read_twice + write_twice;
  };

  REQUIRE(run(reading()) == 16);
  REQUIRE(*frames == 0);
}

TEST_CASE("any_stream: an awaitable held past its end leaves room for the next", "[io][any_stream]") {
  auto const frames = std::make_shared<int>(0);
  std::array<std::byte, 4> buffer{};

  auto const reading = [&]() -> task<std::size_t> {
    io::any_stream stream{task_stream{frames}};
    auto first = stream.read_some(buffer);
    auto const one = co_await first;
    auto second = stream.read_some(buffer);
    auto const two = co_await second;

    co_return one.transferred + two.transferred;
  };

  REQUIRE(run(reading()) == 8);
  REQUIRE(*frames == 0);
}

TEST_CASE("any_stream: an operation over one still pending is refused", "[io][any_stream]") {
  std::array<std::byte, 4> buffer{};
  io::any_stream stream{task_stream{}};

  {
    auto const read = stream.read_some(buffer);

    REQUIRE_THROWS_AS((void)stream.read_some(buffer), std::logic_error);
    REQUIRE_THROWS_AS((void)stream.shutdown_send(), std::logic_error);
  }

  {
    auto const written = stream.write_some(buffer);

    REQUIRE_THROWS_AS((void)stream.write_some(buffer), std::logic_error);
    REQUIRE_THROWS_AS((void)stream.shutdown_send(), std::logic_error);
    REQUIRE_NOTHROW((void)stream.read_some(buffer));
  }

  {
    auto const stopping = stream.shutdown_send();

    REQUIRE_THROWS_AS((void)stream.read_some(buffer), std::logic_error);
    REQUIRE_THROWS_AS((void)stream.write_some(buffer), std::logic_error);
    REQUIRE_THROWS_AS((void)stream.shutdown_send(), std::logic_error);
  }

  REQUIRE_NOTHROW((void)stream.read_some(buffer));
}

TEST_CASE("any_stream: an operation on one moved from is refused", "[io][any_stream]") {
  std::array<std::byte, 4> buffer{};
  io::any_stream stream{task_stream{}};
  io::any_stream const moved{std::move(stream)};

  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what is tested.
  REQUIRE_THROWS_AS((void)stream.read_some(buffer), std::logic_error);
  REQUIRE_THROWS_AS((void)stream.write_some(buffer), std::logic_error);
  REQUIRE_THROWS_AS((void)stream.shutdown_send(), std::logic_error);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

TEST_CASE("any_stream: awaiting an operation twice is refused", "[io][any_stream]") {
  std::array<std::byte, 4> buffer{};

  auto const twice = [&]() -> task<bool> {
    io::any_stream stream{task_stream{}};
    auto read = stream.read_some(buffer);
    auto const first = co_await read;

    static_cast<void>(first);

    try {
      auto const second = co_await read;

      static_cast<void>(second);
    } catch (std::logic_error const &) {
      co_return true;
    }

    co_return false;
  };

  REQUIRE(run(twice()) == true);
}

TEST_CASE("any_stream: an operation that throws leaves room for the next", "[io][any_stream]") {
  struct throwing_stream {
    [[nodiscard]] static task<io::io_result> failing() {
      throw std::runtime_error{"on purpose"};
      co_return io::io_result{};
    }

    [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { return failing(); }

    [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) { return failing(); }
  };

  std::array<std::byte, 4> buffer{};

  auto const reading = [&]() -> task<int> {
    io::any_stream stream{throwing_stream{}};
    auto failures = 0;

    for (auto attempt = 0; attempt < 2; ++attempt) {
      try {
        auto const read = co_await stream.read_some(buffer);

        static_cast<void>(read);
      } catch (std::runtime_error const &) {
        ++failures;
      }
    }

    co_return failures;
  };

  REQUIRE(run(reading()) == 2);
}

TEST_CASE("any_stream: a read reports what can be trusted, whoever reads", "[io][any_stream]") {
  struct misreporting_stream {
    io::io_result answer;

    [[nodiscard]] static task<io::io_result> done(io::io_result result) { co_return result; }

    [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { return done(answer); }

    [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) { return done(answer); }
  };

  std::array<std::byte, 4> buffer{};

  auto const reading = [&](io::io_result answer) -> task<io::io_result> {
    io::any_stream stream{misreporting_stream{answer}};

    co_return co_await stream.read_some(buffer);
  };

  auto const past = run(reading({{}, 5}));
  auto const nothing = run(reading({{}, 0}));
  auto const fine = run(reading({{}, 4}));

  REQUIRE(past->error == std::errc::io_error);
  REQUIRE(past->transferred == 0);
  REQUIRE(nothing->error == std::errc::io_error);
  REQUIRE_FALSE(fine->error);
  REQUIRE(fine->transferred == 4);
}

TEST_CASE("any_stream: target of what is no stream is nothing", "[io][any_stream]") {
  io::any_stream stream{task_stream{}};
  auto const &same = stream;

  REQUIRE(stream.target<int>() == nullptr);
  REQUIRE(same.target<int>() == nullptr);
  REQUIRE(stream.target<task_stream>() != nullptr);
}

TEST_CASE("any_stream: an over-aligned stream is held where it can be", "[io][any_stream]") {
  io::any_stream stream{aligned_stream{}};
  std::array<std::byte, 1> buffer{};

  REQUIRE(reinterpret_cast<std::uintptr_t>(stream.target<aligned_stream>()) % alignof(aligned_stream) == 0);

  auto const reading = [&]() -> task<std::size_t> { co_return (co_await stream.read_some(buffer)).transferred; };

  REQUIRE(run(reading()) == 1);
}

TEST_CASE("any_stream: a stream replaced is gone, and only it", "[io][any_stream]") {
  auto const first = std::make_shared<int>(0);
  auto const second = std::make_shared<int>(0);

  {
    io::any_stream stream{counted_stream{first}};

    REQUIRE(*first == 0);
    stream = io::any_stream{counted_stream{second}};
    REQUIRE(*first == 1);
    REQUIRE(*second == 0);
  }

  REQUIRE(*first == 1);
  REQUIRE(*second == 1);
}

TEST_CASE("io::error: says what it is", "[io]") {
  auto const code = std::make_error_code(std::errc::io_error);
  std::error_code const ended = io::error::end_of_stream;

  REQUIRE(std::string_view{ended.category().name()} == "quoll.io");
  REQUIRE_FALSE(ended.message().empty());
  REQUIRE(ended.category().message(-1) == "unknown quoll.io error");
  REQUIRE(ended != code);
}
