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
#include <quoll/async/yield.hpp>
#include <quoll/io.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace io = quoll::io;

using quoll::async::task;

template <class... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
};

template <class T>
std::optional<T> run(task<T> work) {
  quoll::async::run_loop loop;
  std::optional<T> result;
  std::exception_ptr failure;

  co_spawn(loop.get_executor(), std::move(work),
           overloaded{[&](T value) { result = std::move(value); },
                      [&](std::exception_ptr error) { failure = std::move(error); }});
  loop.run();

  // Rethrown here rather than failing in the handler, which must not throw: Catch2 reports the message.
  if (failure) {
    std::rethrow_exception(failure);
  }

  return result;
}

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
