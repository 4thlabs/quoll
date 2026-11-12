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

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/is_event_stream.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;

// A body that gives a byte a read, a turn of the loop after it is asked, as a slow peer would.
class byte_source {
public:
  explicit byte_source(std::string text) : text_(std::move(text)) {}

  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    co_await async::yield();
    ++reads_;

    // As a transport's body does: a stop ends the read.
    if ((co_await async::current_stop_token()).stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled)};
    }

    if (at_ == text_.size()) {
      co_return std::nullopt;
    }

    co_return std::string_view{text_}.substr(at_++, 1);
  }

  [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
  std::string text_;
  std::size_t at_ = 0;
  std::size_t reads_ = 0;
};

struct read_all {
  std::vector<http::server_sent_event> events;
  std::string last_event_id;
};

async::task<read_all> events_of(http::response_body body) {
  http::event_reader events{std::move(body)};
  read_all out;

  while (auto event = co_await events.next()) {
    out.events.push_back(std::move(*event));
  }

  out.last_event_id = events.last_event_id();
  co_return out;
}

} // namespace

TEST_CASE("event_reader: events read a byte a piece, cut in a field and in a CRLF", "[http][sse]") {
  std::string const text = "id: 7\r\nevent: add\r\ndata: a\r\n\r\ndata: b\n\ndata: lost";
  auto const source = std::make_shared<byte_source>(text);
  auto const got = quoll::test::run(events_of(http::response_body{source}));

  REQUIRE(got);
  REQUIRE(
      got->events ==
      std::vector<http::server_sent_event>{{.type = "add", .data = "a", .id = "7"}, {.data = "b", .id = "7"}}
  );
  REQUIRE(got->last_event_id == "7");
  // A piece a byte, and the end.
  REQUIRE(source->reads() == text.size() + 1);
}

TEST_CASE("event_reader: a failure lets the body go, and comes again", "[http][sse]") {
  auto source = std::make_shared<byte_source>("data: " + std::string(64, 'x') + "\n\n");
  http::event_reader events{http::response_body{source}, 16};

  auto const failure = [&events]() -> async::task<std::error_code> {
    try {
      static_cast<void>(co_await events.next());
    } catch (std::system_error const &failed) {
      co_return failed.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(quoll::test::run(failure()) == http::error::event_too_large);
  // Only the test holds the source now: the reader let its body go.
  REQUIRE(source.use_count() == 1);
  REQUIRE(quoll::test::run(failure()) == http::error::event_too_large);
}

TEST_CASE("is_event_stream: the media type, case and parameters aside", "[http][sse]") {
  auto const with = [](std::string_view type) {
    http::fields fields;

    fields.set("Content-Type", type);
    return http::is_event_stream(fields);
  };

  REQUIRE(with("text/event-stream"));
  REQUIRE(with("Text/Event-Stream"));
  REQUIRE(with("text/event-stream; charset=utf-8"));
  REQUIRE(with("text/event-stream ;charset=utf-8"));
  REQUIRE_FALSE(with("text/html"));
  REQUIRE_FALSE(with("text/event-streams"));
  REQUIRE_FALSE(with(""));
  REQUIRE_FALSE(http::is_event_stream(http::fields{}));
}

TEST_CASE("event_reader: a stop request ends the reading, and lets the body go", "[http][sse]") {
  auto source = std::make_shared<byte_source>("data: a\n\n");
  http::event_reader events{http::response_body{source}};
  std::stop_source stop;
  async::run_loop loop;

  stop.request_stop();

  REQUIRE(
      quoll::test::run(loop, quoll::test::code_thrown_by(events.next()), stop.get_token()) ==
      std::errc::operation_canceled
  );
  REQUIRE(source.use_count() == 1);
}

TEST_CASE(
    "event_reader: a next() asked while another is, not yet awaited, is refused; one dropped frees it", "[http][sse]"
) {
  auto source = std::make_shared<byte_source>("data: a\n\ndata: b\n\n");
  http::event_reader events{http::response_body{source}};

  auto const reading = [&events]() -> async::task<bool> {
    auto first = events.next();
    bool refused = false;

    try {
      static_cast<void>(events.next());
    } catch (std::logic_error const &) {
      refused = true;
    }

    { auto const dropped = std::move(first); }

    // Its turn given back unrun: the next one reads the first event.
    auto const event = co_await events.next();

    co_return refused &&event == http::server_sent_event{.data = "a"};
  };

  REQUIRE(quoll::test::run(reading()) == true);
}

namespace {

// A body that never says anything.
class silent_source {
public:
  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    co_await async::sleep_for(std::chrono::hours{24});
    co_return std::nullopt;
  }
};

} // namespace

TEST_CASE("event_reader: a timeout around next() ends the reading", "[http][sse][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::event_reader events{http::response_body{std::make_shared<silent_source>()}};

  auto const reading = [&events]() -> async::task<std::string> {
    std::string seen;

    try {
      static_cast<void>(co_await async::with_timeout(1s, events.next()));
    } catch (std::system_error const &failed) {
      seen += failed.code() == std::errc::timed_out ? "timed out;" : "other;";
    }

    // Ended, as the body's reading is once cut: the same failure, not a wait.
    try {
      static_cast<void>(co_await events.next());
    } catch (std::system_error const &) {
      seen += "ended";
    }

    co_return seen;
  };

  REQUIRE(quoll::test::run(time, reading()) == "timed out;ended");
}

namespace {

// A body whose read fails with what is no system_error.
class failing_source {
public:
  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    co_await async::yield();
    throw std::runtime_error{"transport"};
  }
};

} // namespace

TEST_CASE("event_reader: any failure of the body ends the reading, and lets it go", "[http][sse]") {
  auto source = std::make_shared<failing_source>();
  http::event_reader events{http::response_body{source}};

  auto const read = [&events]() -> async::task<int> {
    int failed = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
      try {
        static_cast<void>(co_await events.next());
      } catch (std::runtime_error const &) {
        ++failed;
      }
    }

    co_return failed;
  };

  REQUIRE(quoll::test::run(read()) == 2);
  REQUIRE(source.use_count() == 1);
}

TEST_CASE("event_reader: a next() over gives its turn back, its task still held", "[http][sse]") {
  auto source = std::make_shared<byte_source>("data: a\n\ndata: b\n\n");
  http::event_reader events{http::response_body{source}};

  auto const reading = [&events]() -> async::task<bool> {
    auto first = events.next();
    auto const a = co_await std::move(first);
    auto const b = co_await events.next();

    co_return a == http::server_sent_event{.data = "a"} && b == http::server_sent_event{.data = "b"};
  };

  REQUIRE(quoll::test::run(reading()) == true);
}
