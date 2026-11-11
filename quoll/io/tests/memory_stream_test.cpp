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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/io.hpp>
#include <quoll/io/detail/sound_read.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace io = quoll::io;

using quoll::async::task;

using quoll::test::overloaded;
using quoll::test::run;

std::span<std::byte const> bytes(std::string_view text) { return std::as_bytes(std::span{text}); }

// What a read brought, as text, with its error.
struct received {
  std::string text;
  std::error_code error;
};

task<received> read_once(io::memory_stream &stream, std::size_t capacity = 64) {
  std::array<std::byte, 64> storage{};
  auto const buffer = std::span{storage}.first(capacity);
  auto const result = co_await stream.read_some(buffer);

  co_return received{std::string{reinterpret_cast<char const *>(buffer.data()), result.transferred}, result.error};
}

task<io::io_result> write_later(io::memory_stream &stream, std::string_view text) {
  co_await quoll::async::yield();
  co_return co_await stream.write_some(bytes(text));
}

task<> close_later(io::memory_stream &stream) {
  co_await quoll::async::yield();
  stream.close();
}

task<io::io_result> write_once(io::memory_stream &stream, std::string_view text) {
  co_return co_await stream.write_some(bytes(text));
}

task<io::io_result> write_all(io::memory_stream &stream, std::string_view text) {
  co_return co_await io::write(stream, bytes(text));
}

task<std::tuple<received, io::io_result>> read_while_peer_writes(io::memory_stream &reader, io::memory_stream &writer) {
  co_return co_await quoll::async::when_all(read_once(reader), write_later(writer, "ping"));
}

task<> fail_later() {
  co_await quoll::async::yield();
  throw std::runtime_error{"sibling failed"};
}

task<std::tuple<received, std::monostate>> read_while_sibling_fails(io::memory_stream &reader) {
  co_return co_await quoll::async::when_all(read_once(reader), fail_later());
}

// Posts to a run_loop, or throws while `failing` is set, as a queue failing to allocate would.
struct flaky_executor {
  quoll::async::run_loop::executor inner;
  bool const *failing;

  void post(quoll::async::work_item &item) const {
    if (*failing) {
      throw std::bad_alloc{};
    }

    inner.post(item);
  }

  [[nodiscard]] bool operator==(flaky_executor const &) const noexcept = default;
};

task<std::tuple<received, std::monostate>> read_while_peer_closes(io::memory_stream &reader,
                                                                  io::memory_stream &writer) {
  co_return co_await quoll::async::when_all(read_once(reader), close_later(writer));
}

} // namespace

TEST_CASE("memory_stream: a read gets what the peer wrote", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  auto const written = run(write_once(right, "hello"));

  REQUIRE(written);
  REQUIRE(!written->error);
  REQUIRE(written->transferred == 5);

  auto const read = run(read_once(left));

  REQUIRE(read);
  REQUIRE(!read->error);
  REQUIRE(read->text == "hello");
}

TEST_CASE("memory_stream: poll_read tells what a read would do, reading nothing", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  static_assert(io::PollableStream<io::memory_stream>);
  REQUIRE(left.poll_read() == io::read_readiness::would_wait);
  REQUIRE(run(write_once(right, "hello")));
  REQUIRE(left.poll_read() == io::read_readiness::ready);
  REQUIRE(left.poll_read() == io::read_readiness::ready);
  REQUIRE(run(read_once(left))->text == "hello");
  REQUIRE(left.poll_read() == io::read_readiness::would_wait);

  right.close();

  REQUIRE(left.poll_read() == io::read_readiness::ready);
  REQUIRE(run(read_once(left))->error == io::error::end_of_stream);
}

TEST_CASE("memory_stream: both directions are independent", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  REQUIRE(run(write_once(left, "to right")));
  REQUIRE(run(write_once(right, "to left")));
  REQUIRE(run(read_once(right))->text == "to right");
  REQUIRE(run(read_once(left))->text == "to left");
}

TEST_CASE("memory_stream: a read waits until the peer writes", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  auto const result = run(read_while_peer_writes(left, right));

  REQUIRE(result);

  auto const &[read, written] = *result;

  REQUIRE(!written.error);
  REQUIRE(!read.error);
  REQUIRE(read.text == "ping");
}

TEST_CASE("memory_stream: a short buffer leaves the rest for the next read", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  REQUIRE(run(write_once(right, "abc")));
  REQUIRE(run(write_once(right, "def")));
  REQUIRE(run(read_once(left, 4))->text == "abcd");
  REQUIRE(run(read_once(left, 4))->text == "ef");
}

TEST_CASE("memory_stream: an empty buffer reads nothing without waiting", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  auto const read = run(read_once(left, 0));

  REQUIRE(read);
  REQUIRE(!read->error);
  REQUIRE(read->text.empty());
}

TEST_CASE("memory_stream: close ends the stream after the bytes already sent", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  REQUIRE(run(write_once(right, "last")));
  right.close();

  REQUIRE(run(read_once(left))->text == "last");
  REQUIRE(run(read_once(left))->error == io::error::end_of_stream);
  REQUIRE(run(write_once(right, "more"))->error == std::errc::broken_pipe);

  // Only the sending side is closed: the other direction still works.
  REQUIRE(!run(write_once(left, "reply"))->error);
  REQUIRE(run(read_once(right))->text == "reply");
}

TEST_CASE("memory_stream: close wakes a pending read", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  auto const result = run(read_while_peer_closes(left, right));

  REQUIRE(result);
  REQUIRE(std::get<0>(*result).error == io::error::end_of_stream);
}

TEST_CASE("memory_stream: destroying an end closes it", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  { auto gone = std::move(right); }

  REQUIRE(run(read_once(left))->error == io::error::end_of_stream);
  REQUIRE(run(write_once(left, "anyone?"))->error == std::errc::broken_pipe);
}

TEST_CASE("memory_stream: a moved end keeps the connection", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  auto moved = std::move(left);

  REQUIRE(run(write_once(right, "still here")));
  REQUIRE(run(read_once(moved))->text == "still here");
}

TEST_CASE("memory_stream: a stop request ends a waiting read", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  std::stop_source source;
  std::optional<received> read;
  auto stopped = false;

  co_spawn(loop.get_executor(), read_once(left),
           overloaded{[&](received value) { read = std::move(value); }, [](std::exception_ptr) {},
                      [&stopped](quoll::async::stopped_t) { stopped = true; }},
           source.get_token());
  loop.run();

  REQUIRE_FALSE(read);

  source.request_stop();
  loop.run();

  // Ended on the stop: the read gives nothing, and what awaited it does not resume.
  REQUIRE(stopped);
  REQUIRE_FALSE(read);

  // The stream is still usable.
  REQUIRE(run(write_once(right, "late")));
  REQUIRE(run(read_once(left))->text == "late");
}

TEST_CASE("memory_stream: a stop request after the bytes arrived loses nothing", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  std::stop_source source;
  std::optional<received> read;

  co_spawn(loop.get_executor(), read_once(left),
           overloaded{[&](received value) { read = std::move(value); }, [](std::exception_ptr) {}}, source.get_token());
  loop.run();

  // The write wakes the read; the stop request comes before it resumes.
  REQUIRE(run(write_once(right, "arrived")));
  source.request_stop();
  loop.run();

  REQUIRE(read);
  REQUIRE(!read->error);
  REQUIRE(read->text == "arrived");
}

TEST_CASE("memory_stream: when_all stops a waiting read when a sibling fails", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();

  REQUIRE_THROWS_AS(run(read_while_sibling_fails(left)), std::runtime_error);
}

TEST_CASE("memory_stream: a write that throws sends nothing", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  bool failing = false;
  std::optional<received> read;

  co_spawn(flaky_executor{loop.get_executor(), &failing}, read_once(left),
           overloaded{[&](received value) { read = std::move(value); }, [](std::exception_ptr) {}});
  loop.run();

  failing = true;

  REQUIRE_THROWS_AS(right.write_some(bytes("data")).await_resume(), std::bad_alloc);

  failing = false;

  REQUIRE(!right.write_some(bytes("data")).await_resume().error);

  loop.run();

  REQUIRE(read);
  REQUIRE(read->text == "data");
}

TEST_CASE("memory_stream: shutdown_send ends what the peer reads, and reading goes on", "[io][memory_stream]") {
  auto [client, server] = quoll::io::make_memory_stream_pair();
  std::string const sent = "bye";
  std::string const answer = "late";

  auto const steps = [&]() -> quoll::async::task<std::string> {
    std::array<char, 16> chunk{};

    (void)co_await quoll::io::write(server, std::as_bytes(std::span{sent}));
    REQUIRE_FALSE((co_await server.shutdown_send()).error);

    auto const first = co_await client.read_some(std::as_writable_bytes(std::span{chunk}));

    REQUIRE(first.transferred == sent.size());
    REQUIRE((co_await client.read_some(std::as_writable_bytes(std::span{chunk}))).error ==
            quoll::io::error::end_of_stream);

    // The other direction is still open.
    (void)co_await quoll::io::write(client, std::as_bytes(std::span{answer}));

    auto const later = co_await server.read_some(std::as_writable_bytes(std::span{chunk}));

    co_return std::string{chunk.data(), later.transferred};
  };

  REQUIRE(quoll::test::run(steps()) == answer);
}

TEST_CASE("io::write writes every byte", "[io][write]") {
  auto [left, right] = io::make_memory_stream_pair();

  auto const written = run(write_all(right, "payload"));

  REQUIRE(written);
  REQUIRE(!written->error);
  REQUIRE(written->transferred == 7);
  REQUIRE(run(read_once(left))->text == "payload");

  left = std::move(right);

  REQUIRE(run(write_all(left, "lost"))->error == std::errc::broken_pipe);
}

namespace {

// Takes at most `piece` bytes a write, and after `until` bytes answers `answer`: what io::write has to make of it.
struct cutting_stream {
  std::size_t piece;
  std::size_t until;
  io::io_result answer;
  std::size_t written = 0;

  [[nodiscard]] static task<io::io_result> done(io::io_result result) { co_return result; }

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { return done({}); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (written >= until) {
      return done(answer);
    }

    auto const size = std::min({piece, data.size(), until - written});

    written += size;
    return done({{}, size});
  }
};

} // namespace

TEST_CASE("io::write: goes on until all has left, and counts what left before an error", "[io]") {
  std::string_view const data = "0123456789";

  SECTION("short writes") {
    cutting_stream stream{.piece = 3, .until = 100, .answer = {}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE_FALSE(result->error);
    REQUIRE(result->transferred == 10);
    REQUIRE(stream.written == 10);
  }

  SECTION("an error part of the way") {
    cutting_stream stream{.piece = 3, .until = 6, .answer = {std::make_error_code(std::errc::broken_pipe), 0}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE(result->error == std::errc::broken_pipe);
    REQUIRE(result->transferred == 6);
  }

  SECTION("an error with bytes of its own") {
    cutting_stream stream{.piece = 3, .until = 6, .answer = {std::make_error_code(std::errc::broken_pipe), 2}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE(result->error == std::errc::broken_pipe);
    REQUIRE(result->transferred == 8);
  }

  SECTION("no error and no progress ends, in every build") {
    cutting_stream stream{.piece = 3, .until = 6, .answer = {}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE(result->error == std::errc::io_error);
    REQUIRE(result->transferred == 6);
  }

  SECTION("an error with more than it was given stays that error") {
    cutting_stream stream{.piece = 3, .until = 6, .answer = {std::make_error_code(std::errc::broken_pipe), 100}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE(result->error == std::errc::broken_pipe);
    REQUIRE(result->transferred == 6);
  }

  SECTION("more than it was given is no count to trust") {
    cutting_stream stream{.piece = 3, .until = 6, .answer = {{}, 100}};
    auto const result = run(io::write(stream, bytes(data)));

    REQUIRE(result->error == std::errc::io_error);
    REQUIRE(result->transferred == 6);
  }
}

TEST_CASE("memory_stream: a loop going on after an error code does not see a stop, which ends it",
          "[io][memory_stream][stopped]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  std::stop_source source;
  auto errors = 0;
  auto turns = 0;
  auto stopped = false;

  // Bounded, so that the trap #151 closes, a loop reading again at once after a stop, ends the test.
  auto const retrying = [&]() -> task<int> {
    std::array<std::byte, 8> buffer{};

    while (turns < 1000) {
      ++turns;

      if ((co_await left.read_some(buffer)).error) {
        ++errors;
      }
    }

    co_return turns;
  };

  co_spawn(loop.get_executor(), retrying(),
           overloaded{[](int) {}, [](std::exception_ptr) {}, [&stopped](quoll::async::stopped_t) { stopped = true; }},
           source.get_token());
  loop.run();
  source.request_stop();
  loop.run();

  REQUIRE(stopped);
  REQUIRE(turns == 1);
  REQUIRE(errors == 0);
}

TEST_CASE("memory_stream: a read stopped, then unwound once another waits, leaves that one waiting",
          "[io][memory_stream][stopped]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  std::stop_source first_stop;
  auto stopped = false;
  std::optional<std::string> second;

  co_spawn(
      loop.get_executor(), read_once(left),
      overloaded{[](received) {}, [](std::exception_ptr) {}, [&stopped](quoll::async::stopped_t) { stopped = true; }},
      first_stop.get_token());
  loop.run();

  // Stops the first read, then waits at once on the same end, before the first is unwound.
  auto const next = [&]() -> task<std::string> {
    first_stop.request_stop();
    co_return (co_await read_once(left)).text;
  };

  co_spawn(loop.get_executor(), next(),
           overloaded{[&second](std::string text) { second = std::move(text); }, [](std::exception_ptr) {}});
  loop.run();
  REQUIRE(stopped);
  REQUIRE_FALSE(second);

  REQUIRE(run(write_once(right, "late")));
  loop.run();
  REQUIRE(second == "late");
}

TEST_CASE("memory_stream: a stop asked for already ends a read that would wait", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  std::stop_source source;
  std::optional<received> read;
  auto stopped = false;

  source.request_stop();
  co_spawn(loop.get_executor(), read_once(left),
           overloaded{[&](received value) { read = std::move(value); }, [](std::exception_ptr) {},
                      [&stopped](quoll::async::stopped_t) { stopped = true; }},
           source.get_token());
  loop.run();

  REQUIRE(stopped);
  REQUIRE_FALSE(read);
}

TEST_CASE("memory_stream: a reader a little behind does not keep all that went through", "[io][memory_stream]") {
  io::detail::memory_channel channel;
  std::array<std::byte, 64> chunk{};

  // A writer always ahead: the buffer never empties.
  (void)channel.write(chunk);

  for (auto round = 0; round < 1000; ++round) {
    (void)channel.write(chunk);
    (void)channel.read(chunk);
  }

  REQUIRE(channel.held() <= 4 * chunk.size());
}

TEST_CASE("memory_stream: a suspended read that goes leaves the peer nothing to find", "[io][memory_stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  quoll::async::run_loop loop;
  auto const executor = loop.get_executor();
  std::array<std::byte, 4> buffer{};

  {
    auto read = left.read_some(buffer);

    REQUIRE_FALSE(read.await_ready());
    read.await_suspend(std::noop_coroutine(),
                       quoll::async::await_context{.executor = quoll::async::executor_ref{executor}, .stop_token = {}});
  }

  // It would otherwise post the read that went, and another could not start.
  REQUIRE(run(write_once(right, "late")));
  loop.run();
  REQUIRE(run(read_once(left))->text == "late");
}

TEST_CASE("io::detail::sound_read: what a stream reports is what can be trusted", "[io]") {
  auto const io_error = std::make_error_code(std::errc::io_error);
  auto const reset = std::make_error_code(std::errc::connection_reset);

  REQUIRE(io::detail::sound_read({{}, 4}, 4).transferred == 4);
  REQUIRE_FALSE(io::detail::sound_read({{}, 4}, 4).error);
  REQUIRE(io::detail::sound_read({{}, 5}, 4).error == io_error);
  REQUIRE(io::detail::sound_read({{}, 5}, 4).transferred == 0);
  REQUIRE(io::detail::sound_read({{}, 0}, 4).error == io_error);
  REQUIRE(io::detail::sound_read({reset, 3}, 4).error == reset);
  REQUIRE(io::detail::sound_read({reset, 3}, 4).transferred == 0);
}
