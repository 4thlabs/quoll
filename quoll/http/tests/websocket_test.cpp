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

#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/http/websocket_parser.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/next_websocket_message.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::websocket_message;
using http::websocket_opcode;
using http::websocket_role;
using quoll::async::task;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

constexpr http::websocket_mask mask{0x12, 0x34, 0x56, 0x78};

// A server's end over `stream`.
http::websocket server_over(io::memory_stream stream, http::websocket_options options = {}) {
  return http::websocket{io::any_stream{std::move(stream)}, websocket_role::server, {}, options};
}

// `frame` written by a peer that speaks no more than bytes.
task<> send_raw(io::memory_stream &stream, std::string frame) {
  (void)co_await io::write(stream, std::as_bytes(std::span{frame}));
}

websocket_message text(std::string data) { return {websocket_opcode::text, std::move(data)}; }

websocket_message close_with(std::uint16_t code, std::string reason = {}) {
  return {websocket_opcode::close, std::move(reason), code};
}

// A stream that writes one byte at a time, each after the others have had their turn: frames sent at once would mix.
class trickling {
public:
  explicit trickling(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    co_await quoll::async::yield();
    co_return co_await inner_.write_some(data.first(std::min<std::size_t>(data.size(), 1)));
  }

private:
  io::memory_stream inner_;
};

// A stream that writes one byte a millisecond, a stop ending a write with nothing written.
class slow_writes {
public:
  explicit slow_writes(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    try {
      co_await quoll::async::sleep_for(1ms);
    } catch (std::system_error const &stopped) {
      co_return io::io_result{stopped.code(), 0};
    }

    co_return co_await inner_.write_some(data.first(std::min<std::size_t>(data.size(), 1)));
  }

private:
  io::memory_stream inner_;
};

// How a breaking_timer breaks: its first wait fails, with an exception that carries no code, or with a code of zero,
// or each wait that is stopped does, rather than return false.
enum class breaks : std::uint8_t { first_wait, first_wait_with_no_code, when_stopped };

// A virtual_time whose timer breaks as `how` says, as a timer that fails would; the rest waits as virtual_time's does.
class breaking_timer {
public:
  class executor {
  public:
    using services = quoll::async::service_list<quoll::async::timer_ref>;

    executor(quoll::test::virtual_time::executor inner, breaks how, int &waits) noexcept
        : inner_(inner), how_(how), waits_(&waits) {}

    void post(quoll::async::work_item &item) const noexcept { inner_.post(item); }

    [[nodiscard]] task<bool> wait_for(std::chrono::nanoseconds duration) const {
      if (how_ == breaks::when_stopped) {
        return failed_when_stopped(inner_, duration);
      }

      if ((*waits_)++ == 0) {
        return how_ == breaks::first_wait ? failed() : failed_with_no_code();
      }

      return inner_.wait_for(duration);
    }

    [[nodiscard]] std::chrono::nanoseconds now() const noexcept { return inner_.now(); }

    [[nodiscard]] quoll::async::timer_ref query(quoll::async::service_tag<quoll::async::timer_ref> /*tag*/) const {
      return quoll::async::timer_ref{*this};
    }

    [[nodiscard]] bool operator==(executor const &) const noexcept = default;

  private:
    static task<bool> failed() {
      throw std::runtime_error{"the timer broke"};
      co_return false;
    }

    static task<bool> failed_with_no_code() {
      throw std::system_error{std::error_code{}, "the timer broke"};
      co_return false;
    }

    // By value: the executor that answered the query may be gone by the time the wait ends.
    static task<bool> failed_when_stopped(quoll::test::virtual_time::executor inner,
                                          std::chrono::nanoseconds duration) {
      if (!co_await inner.wait_for(duration)) {
        throw std::runtime_error{"the timer broke as it was stopped"};
      }

      co_return true;
    }

    quoll::test::virtual_time::executor inner_;
    breaks how_;
    int *waits_;
  };

  explicit breaking_timer(breaks how) noexcept : how_(how) {}

  [[nodiscard]] executor get_executor() noexcept { return executor{time_.get_executor(), how_, waits_}; }

  void run() { time_.run(); }

private:
  quoll::test::virtual_time time_;
  breaks how_;
  int waits_ = 0;
};

// A stream whose read throws once stopped, rather than say so, as a stream that breaks would.
class breaking_read {
public:
  explicit breaking_read(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
    auto const read = co_await inner_.read_some(buffer);

    if (read.error == std::errc::operation_canceled) {
      throw std::runtime_error{"the read broke as it was stopped"};
    }

    co_return read;
  }

  [[nodiscard]] auto write_some(std::span<std::byte const> data) { return inner_.write_some(data); }

private:
  io::memory_stream inner_;
};

// A stream that reads what its peer sends, and whose writes take a byte every `delay`, as a peer slow to read does.
class taking_slowly {
public:
  taking_slowly(io::memory_stream inner, std::chrono::nanoseconds delay) noexcept
      : inner_(std::move(inner)), delay_(delay) {}

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    co_await quoll::async::sleep_for(delay_);
    co_return co_await inner_.write_some(data.first(std::min<std::size_t>(data.size(), 1)));
  }

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

private:
  io::memory_stream inner_;
  std::chrono::nanoseconds delay_;
};

// A stream that reads what its peer sends, and whose writes fail as on a connection the peer reset, after `delay`.
class reset_late {
public:
  reset_late(io::memory_stream inner, std::chrono::nanoseconds delay) noexcept
      : inner_(std::move(inner)), delay_(delay) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> /*data*/) const {
    co_await quoll::async::detail::stop_scope<void>{quoll::async::sleep_for(delay_), std::stop_token{}};
    co_return io::io_result{std::make_error_code(std::errc::connection_reset), 0};
  }

private:
  io::memory_stream inner_;
  std::chrono::nanoseconds delay_;
};

// A stream that reads what its peer sends, and whose writes throw a failure with a code of zero.
class throwing_on_write {
public:
  explicit throwing_on_write(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] static task<io::io_result> write_some(std::span<std::byte const> /*data*/) {
    throw std::system_error{std::error_code{}, "the write broke"};
    co_return io::io_result{};
  }

private:
  io::memory_stream inner_;
};

// A stream that reads what its peer sends, and whose writes fail as on a connection the peer reset.
class reset_on_write {
public:
  explicit reset_on_write(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] static task<io::io_result> write_some(std::span<std::byte const> /*data*/) {
    co_return io::io_result{std::make_error_code(std::errc::connection_reset), 0};
  }

private:
  io::memory_stream inner_;
};

} // namespace

TEST_CASE("websocket: messages go both ways, masked by a client and not by a server", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client};

  auto const talk = [&]() -> task<int> {
    co_await client.send_text("hello");
    co_await client.send_binary("\x00\xff"sv);

    auto const first = co_await server.receive();
    auto const second = co_await server.receive();

    co_await server.send_text(first.data + "!");

    auto const answer = co_await client.receive();

    REQUIRE(first == text("hello"));
    REQUIRE(second == websocket_message{websocket_opcode::binary, std::string{"\x00\xff"sv}});
    REQUIRE(answer == text("hello!"));
    co_return 1;
  };

  // A server refuses what is not masked, a client what is: each read the other's frames.
  REQUIRE(quoll::test::run(talk()) == 1);
}

TEST_CASE("websocket: a client masks what it sends on its own, its pong and its close", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client};
  // A server's end reads only masked frames.
  http::websocket_parser from_client{websocket_role::server};

  auto const answered = [&]() -> task<int> {
    co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::ping, "?"));
    co_await send_raw(server_end, http::format_websocket_close(1000));

    auto const closed = co_await client.receive();
    auto const pong = co_await quoll::test::next_websocket_message(server_end, from_client);
    auto const close = co_await quoll::test::next_websocket_message(server_end, from_client);

    REQUIRE(closed == close_with(1000));
    REQUIRE(pong == websocket_message{websocket_opcode::pong, "?"});
    REQUIRE(close == close_with(1000));
    co_return 1;
  };

  REQUIRE(quoll::test::run(answered()) == 1);
}

TEST_CASE("websocket: what came with the handshake is read first", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket server{io::any_stream{std::move(server_end)}, websocket_role::server,
                         http::format_websocket_frame(websocket_opcode::text, "early", true, mask)};

  auto const early = [&]() -> task<websocket_message> { co_return co_await server.receive(); };

  REQUIRE(quoll::test::run(early()) == text("early"));
}

TEST_CASE("websocket: a ping is answered with a pong, and a pong is dropped", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));
  http::websocket_parser from_server{websocket_role::client};

  auto const pinged = [&]() -> task<int> {
    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::ping, "are you there", true, mask));
    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::pong, "", true, mask));
    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::text, "after", true, mask));

    auto const received = co_await server.receive();
    auto const answer = co_await quoll::test::next_websocket_message(client_end, from_server);

    REQUIRE(received == text("after"));
    REQUIRE(answer == websocket_message{websocket_opcode::pong, "are you there"});
    co_return 1;
  };

  REQUIRE(quoll::test::run(pinged()) == 1);
}

TEST_CASE("websocket: the peer's close is answered, and is the last message", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));
  http::websocket_parser from_server{websocket_role::client};

  SECTION("with a code, answered with that code") {
    auto const closed = [&]() -> task<int> {
      co_await send_raw(client_end, http::format_websocket_close(1001, "going", mask));

      auto const received = co_await server.receive();
      auto const answer = co_await quoll::test::next_websocket_message(client_end, from_server);
      auto const late = co_await quoll::test::code_thrown_by(server.send_text("late"));

      REQUIRE(received == close_with(1001, "going"));
      REQUIRE(answer == close_with(1001));
      REQUIRE(late == http::error::websocket_closed);

      // Sent once: a close() after it sends nothing more.
      co_await server.close();

      // A server ends its side first, while the connection is still there: the peer reads to its end.
      auto const rest = co_await quoll::test::read_until_closed(client_end);

      REQUIRE(rest.empty());
      co_return 1;
    };

    REQUIRE(quoll::test::run(closed()) == 1);
    REQUIRE_THROWS_AS(server.receive(), std::logic_error);
  }

  SECTION("with none, answered with none") {
    auto const closed = [&]() -> task<int> {
      co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::close, "", true, mask));

      auto const received = co_await server.receive();
      auto const answer = co_await quoll::test::next_websocket_message(client_end, from_server);

      REQUIRE(received == close_with(1005));
      REQUIRE(answer == close_with(1005));
      co_return 1;
    };

    REQUIRE(quoll::test::run(closed()) == 1);
  }

  // The close handshake over, the server's side is shut, while the server still holds the connection: nothing else
  // came.
  auto const rest = [&]() -> task<std::string> { co_return co_await quoll::test::read_until_closed(client_end); };

  REQUIRE(quoll::test::run(rest()) == "");
}

TEST_CASE("websocket: close() sends a close once, and nothing after it", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));
  http::websocket_parser from_server{websocket_role::client};

  auto const closing = [&]() -> task<int> {
    co_await server.close(4000, "done");
    co_await server.close();

    auto const late = co_await quoll::test::code_thrown_by(server.send_binary("late"));
    auto const late_ping = co_await quoll::test::code_thrown_by(server.ping());
    auto const sent = co_await quoll::test::next_websocket_message(client_end, from_server);

    REQUIRE(late == http::error::websocket_closed);
    REQUIRE(late_ping == http::error::websocket_closed);
    REQUIRE(sent == close_with(4000, "done"));

    // The peer's answer comes to receive(), and is not answered again.
    co_await send_raw(client_end, http::format_websocket_close(4000, "", mask));

    auto const answer = co_await server.receive();

    REQUIRE(answer == close_with(4000));
    co_return 1;
  };

  REQUIRE(quoll::test::run(closing()) == 1);

  // The close handshake over, the server's side is shut: nothing else came.
  auto const rest = [&]() -> task<std::string> { co_return co_await quoll::test::read_until_closed(client_end); };

  REQUIRE(quoll::test::run(rest()) == "");
}

TEST_CASE("websocket: a peer that breaks the protocol is sent the close it calls for", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket_parser from_server{websocket_role::client};
  http::websocket_options options;
  std::string frame;
  std::error_code expected;
  std::uint16_t code = 0;

  SECTION("a frame that is not masked") {
    frame = http::format_websocket_frame(websocket_opcode::text, "plain");
    expected = http::error::websocket_protocol;
    code = 1002;
  }

  SECTION("text that is not UTF-8") {
    frame = http::format_websocket_frame(websocket_opcode::binary, "\xc3"sv, true, mask).replace(0, 1, "\x81");
    expected = http::error::websocket_invalid_text;
    code = 1007;
  }

  SECTION("a message larger than the options allow") {
    options.max_message_size = 4;
    frame = http::format_websocket_frame(websocket_opcode::binary, "12345", true, mask);
    expected = http::error::websocket_too_large;
    code = 1009;
  }

  auto server = server_over(std::move(server_end), options);

  auto const broken = [&]() -> task<int> {
    co_await send_raw(client_end, frame);

    auto const thrown = co_await quoll::test::code_thrown_by(server.receive());
    auto const sent = co_await quoll::test::next_websocket_message(client_end, from_server);
    // Receiving is over, and says so again.
    auto const again = co_await quoll::test::code_thrown_by(server.receive());

    REQUIRE(thrown == expected);
    REQUIRE(sent == close_with(code));
    REQUIRE(again == expected);
    co_return 1;
  };

  REQUIRE(quoll::test::run(broken()) == 1);
}

TEST_CASE("websocket: a peer gone ends receiving with the stream's error", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));

  { auto const gone = std::move(client_end); }

  auto const receiving = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(server.receive());
  };

  REQUIRE(quoll::test::run(receiving()) == io::error::end_of_stream);
}

namespace {

// A stream whose reads bring nothing, and say nothing of why.
class empty_reads {
public:
  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) { co_return io::io_result{}; }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    co_return io::io_result{{}, data.size()};
  }
};

} // namespace

TEST_CASE("websocket: a read that brings nothing, with no error, ends receiving", "[http][websocket]") {
  http::websocket server{io::any_stream{empty_reads{}}, websocket_role::server};

  auto const receiving = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(server.receive());
  };

  REQUIRE(quoll::test::run(receiving()) == std::errc::io_error);
}

namespace {

// A stream whose first write takes a byte as the operation is stopped, and says so: a frame left in its middle.
class cut_after_a_byte {
public:
  cut_after_a_byte(io::memory_stream inner, std::stop_source &stop) noexcept : inner_(std::move(inner)), stop_(&stop) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (std::exchange(first_, false)) {
      auto const sent = co_await inner_.write_some(data.first(1));

      stop_->request_stop();
      co_return io::io_result{std::make_error_code(std::errc::operation_canceled), sent.transferred};
    }

    co_return co_await inner_.write_some(data);
  }

private:
  io::memory_stream inner_;
  std::stop_source *stop_;
  bool first_ = true;
};

} // namespace

TEST_CASE("websocket: what left of a frame before an error leaves it cut", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::stop_source stop;
  http::websocket server{io::any_stream{cut_after_a_byte{std::move(server_end), stop}}, websocket_role::server};
  quoll::async::run_loop loop;

  // Stopped by its own operation's stop, as a byte had left.
  REQUIRE(quoll::test::run(loop, quoll::test::code_thrown_by(server.send_text("cut")), stop.get_token()) ==
          std::errc::operation_canceled);
  REQUIRE(quoll::test::run(quoll::test::code_thrown_by(server.send_text("next"))) == http::error::websocket_closed);
}

TEST_CASE("websocket: messages leave whole, in the order asked, while a receive() waits", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket server{io::any_stream{trickling{std::move(server_end)}}, websocket_role::server};
  http::websocket_parser from_server{websocket_role::client};

  auto const peer = [&]() -> task<int> {
    auto const first = co_await quoll::test::next_websocket_message(client_end, from_server);
    auto const second = co_await quoll::test::next_websocket_message(client_end, from_server);
    auto const third = co_await quoll::test::next_websocket_message(client_end, from_server);

    REQUIRE(first == text("first"));
    REQUIRE(second == websocket_message{websocket_opcode::binary, "second"});
    REQUIRE(third == websocket_message{websocket_opcode::ping, "third"});

    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::text, "over", true, mask));
    co_return 1;
  };

  auto const both_ways = [&]() -> task<int> {
    auto [received, first, second, third, done] = co_await quoll::async::when_all(
        server.receive(), server.send_text("first"), server.send_binary("second"), server.ping("third"), peer());

    REQUIRE(received == text("over"));
    co_return done;
  };

  REQUIRE(quoll::test::run(both_ways()) == 1);
}

TEST_CASE("websocket: a frame cut short ends sending", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket server{io::any_stream{slow_writes{std::move(server_end)}}, websocket_role::server};

  auto const cut = [&]() -> task<int> {
    auto const cut_short =
        co_await quoll::test::code_thrown_by(quoll::async::with_timeout(2500us, server.send_text("a long message")));
    auto const next = co_await quoll::test::code_thrown_by(server.send_text("next"));

    REQUIRE(cut_short == std::errc::timed_out);
    REQUIRE(next == http::error::websocket_closed);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, cut()) == 1);
}

TEST_CASE("websocket: one receive() at a time, refused when it is asked", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));
  auto pending = server.receive();

  REQUIRE_THROWS_AS(server.receive(), std::logic_error);

  // Its turn given back when it goes, even unrun.
  { auto const dropped = std::move(pending); }

  auto const again = [&]() -> task<websocket_message> {
    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::text, "now", true, mask));
    co_return co_await server.receive();
  };

  REQUIRE(quoll::test::run(again()) == text("now"));
}

TEST_CASE("websocket: copies are the same connection, and an operation outlives them all", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::optional<http::websocket> server{server_over(std::move(server_end))};
  std::optional<http::websocket> copy{*server};
  auto receiving = std::optional{server->receive()};

  // One receive() at a time, whichever copy asks.
  REQUIRE_THROWS_AS(copy->receive(), std::logic_error);

  auto const outlived = [&]() -> task<websocket_message> {
    server.reset();
    copy.reset();
    co_await send_raw(client_end, http::format_websocket_frame(websocket_opcode::text, "still", true, mask));
    co_return co_await std::move(*receiving);
  };

  REQUIRE(quoll::test::run(outlived()) == text("still"));
}

TEST_CASE("websocket: the stream closes once the last copy, and the last operation, has gone", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::optional<http::websocket> server{server_over(std::move(server_end))};
  std::optional<http::websocket> copy{*server};
  std::optional<task<>> sending{copy->send_text("last")};
  http::websocket_parser from_server{websocket_role::client};

  server.reset();
  copy.reset();

  auto const after_copies = [&]() -> task<websocket_message> {
    co_return co_await quoll::test::next_websocket_message(client_end, from_server);
  };

  // The operation still holds the connection, and sends.
  REQUIRE(quoll::test::run([&]() -> task<int> {
            co_await std::move(*sending);
            co_return 1;
          }()) == 1);
  REQUIRE(quoll::test::run(after_copies()) == text("last"));

  sending.reset();

  auto const rest = [&]() -> task<std::string> { co_return co_await quoll::test::read_until_closed(client_end); };

  // No close was sent: the stream itself ends.
  REQUIRE(quoll::test::run(rest()) == "");
}

TEST_CASE("websocket: local_transport, with no connection to hand over, answers 500 and keeps no upgrade",
          "[http][websocket]") {
  http::router router;
  http::request handshake;

  router.get("/socket", [](http::request request) {
    return http::accept_websocket(request, [](http::websocket) -> task<> { co_return; });
  });
  handshake.target = "/socket";
  handshake.fields.add("Host", "h");
  handshake.fields.add("Upgrade", "websocket");
  handshake.fields.add("Connection", "Upgrade");
  handshake.fields.add("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  handshake.fields.add("Sec-WebSocket-Version", "13");

  // A 101 is no final response: replaced with a 500, which carries nothing of it.
  auto const answer = quoll::test::run(http::local_transport{router}.send(handshake));

  REQUIRE(answer->status == http::status::internal_server_error);
  REQUIRE_FALSE(answer->upgrade);
}

TEST_CASE("websocket: what cannot be sent is refused when it is asked", "[http][websocket]") {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto server = server_over(std::move(server_end));

  REQUIRE_THROWS_AS(server.send_text("\xc3"sv), std::invalid_argument);
  REQUIRE_THROWS_AS(server.ping(std::string(126, 'p')), std::invalid_argument);
  REQUIRE_THROWS_AS(server.close(1005), std::invalid_argument);
  REQUIRE_THROWS_AS(server.close(1000, std::string(124, 'r')), std::invalid_argument);
}

TEST_CASE("websocket: a client's receive() pings a quiet peer, then takes it as gone", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};
  std::chrono::nanoseconds pinged_at{};
  std::chrono::nanoseconds gone_at{};

  auto const quiet = [&]() -> task<std::error_code> {
    auto const peer = [&]() -> task<websocket_message> {
      auto ping = co_await quoll::test::next_websocket_message(server_end, from_client);

      pinged_at = time.now();
      co_return ping;
    };
    auto const receiving = [&]() -> task<std::error_code> {
      auto const code = co_await quoll::test::code_thrown_by(client.receive());

      gone_at = time.now();
      co_return code;
    };

    auto [ping, code] = co_await quoll::async::when_all(peer(), receiving());

    REQUIRE(ping == websocket_message{websocket_opcode::ping, ""});
    co_return code;
  };

  REQUIRE(quoll::test::run(time, quiet()) == std::errc::timed_out);
  REQUIRE(pinged_at == 1s);
  REQUIRE(gone_at == 2s);

  // Receiving has ended.
  auto const again = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(time, again()) == std::errc::timed_out);
}

TEST_CASE("websocket: a client's receive() waits on for a peer that answers its pings", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};

  std::vector<std::chrono::nanoseconds> pinged_at;

  auto const alive = [&]() -> task<websocket_message> {
    auto const peer = [&]() -> task<> {
      for (auto pings = 0; pings != 3; ++pings) {
        auto const ping = co_await quoll::test::next_websocket_message(server_end, from_client);

        pinged_at.push_back(time.now());
        co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::pong, ping.data));
      }

      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "late"));
    };

    auto [answered, message] = co_await quoll::async::when_all(peer(), client.receive());

    co_return message;
  };

  REQUIRE(quoll::test::run(time, alive()) == text("late"));
  // Each pong is what the next tick finds read: pinged every other interval.
  REQUIRE(pinged_at == std::vector<std::chrono::nanoseconds>{1s, 3s, 5s});
}

TEST_CASE("websocket: a client's watch judges the peer only while a receive() reads", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};

  // A message, then ten seconds with no receive(), a quiet peer included: nothing reads a pong then, so nothing is
  // judged; the next receive() starts the watch again, pinged a second later, gone a second after.
  auto const paused = [&]() -> task<std::chrono::nanoseconds> {
    co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "first"));

    auto const first = co_await client.receive();

    REQUIRE(first == text("first"));
    co_await quoll::async::sleep_for(10s);

    auto const resumed_at = time.now();
    auto const code = co_await quoll::test::code_thrown_by(client.receive());

    REQUIRE(code == std::errc::timed_out);
    co_return time.now() - resumed_at;
  };

  auto const after = quoll::test::run(time, paused()).value();

  // From the second receive()'s first read: pinged at 1 s, gone at 2 s.
  REQUIRE(after == 2s);
}

TEST_CASE("websocket: with no ping interval, a client's receive() waits as long as the peer is quiet",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 0s}};

  auto const patient = [&]() -> task<websocket_message> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(1h);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "at last"));
    };

    auto [sent, message] = co_await quoll::async::when_all(peer(), client.receive());

    co_return message;
  };

  REQUIRE(quoll::test::run(time, patient()) == text("at last"));
  REQUIRE(time.now() == 1h);
}

TEST_CASE("websocket: a client's receive() after close() takes a peer that does not answer as gone",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};

  auto const unanswered = [&]() -> task<std::error_code> {
    co_await client.close();

    auto const close = co_await quoll::test::next_websocket_message(server_end, from_client);

    REQUIRE(close == close_with(1000));
    // Its answer waited for as a pong is: gone one interval later, with no ping.
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(time, unanswered()) == std::errc::timed_out);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("websocket: a client's receive() ends with its caller's stop, past a ping", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};
  std::optional<websocket_message> ping;
  std::stop_source stop;

  // Stopped past the ping, before the peer would be taken as gone.
  auto const stopped = [&]() -> task<std::error_code> {
    auto const stopping = [&]() -> task<> {
      co_await quoll::async::sleep_for(1500ms);
      stop.request_stop();
    };
    auto const receiving = [&]() -> task<websocket_message> {
      co_return co_await quoll::async::detail::stop_scope<websocket_message>{client.receive(), stop.get_token()};
    };
    auto const peer = [&]() -> task<> { ping = co_await quoll::test::next_websocket_message(server_end, from_client); };

    auto [done, code, read] =
        co_await quoll::async::when_all(stopping(), quoll::test::code_thrown_by(receiving()), peer());

    co_return code;
  };

  REQUIRE(quoll::test::run(time, stopped()) == std::errc::operation_canceled);
  REQUIRE(ping == websocket_message{websocket_opcode::ping, ""});
}

TEST_CASE("websocket: a client's ping that cannot leave ends its receive() with the stream's failure",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{reset_on_write{std::move(client_end)}}, websocket_role::client, {}, {.ping_interval = 1s}};

  auto const reset = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(time, reset()) == std::errc::connection_reset);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("websocket: a client's receive() never waits for its ping, which goes whole", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{taking_slowly{std::move(client_end), 400ms}}, websocket_role::client, {}, {.ping_interval = 1s}};
  std::stop_source stop;
  std::chrono::nanoseconds ended_at{};

  // The ping starts at 1 s, its six bytes taken one every 400 ms; the receive() is stopped at 1.5 s, and ends then.
  auto const stopped = [&]() -> task<std::error_code> {
    auto const stopping = [&]() -> task<> {
      co_await quoll::async::sleep_for(1500ms);
      stop.request_stop();
    };
    auto const receiving = [&]() -> task<std::error_code> {
      auto const code = co_await quoll::test::code_thrown_by([&]() -> task<websocket_message> {
        co_return co_await quoll::async::detail::stop_scope<websocket_message>{client.receive(), stop.get_token()};
      }());

      ended_at = time.now();
      co_return code;
    };

    auto [done, code] = co_await quoll::async::when_all(stopping(), receiving());

    co_return code;
  };

  REQUIRE(quoll::test::run(time, stopped()) == std::errc::operation_canceled);
  REQUIRE(ended_at == 1500ms);

  // Whole: the next frame on the stream reads as one.
  http::websocket_parser from_client{websocket_role::server};
  auto const read = [&]() -> task<websocket_message> {
    co_return co_await quoll::test::next_websocket_message(server_end, from_client);
  };

  REQUIRE(quoll::test::run(time, read()) == websocket_message{websocket_opcode::ping, ""});
}

TEST_CASE("websocket: a client's message comes while its ping waits behind a slow send", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{taking_slowly{std::move(client_end), 1900ms}}, websocket_role::client, {}, {.ping_interval = 2s}};
  std::chrono::nanoseconds received_at{};

  // The send holds the turn for some 20 s, a byte every 1.9 s; the ping due at 2 s waits for it, and the message that
  // comes at 3 s is received at once.
  auto const behind = [&]() -> task<websocket_message> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(3s);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "now"));
    };
    auto const receiving = [&]() -> task<websocket_message> {
      auto message = co_await client.receive();

      received_at = time.now();
      co_return message;
    };

    auto [sent, message, left] = co_await quoll::async::when_all(client.send_text("long"), receiving(), peer());

    co_return message;
  };

  REQUIRE(quoll::test::run(time, behind()) == text("now"));
  REQUIRE(received_at == 3s);
}

TEST_CASE("websocket: a client's keep-alive that fails ends its receive(), and receiving", "[http][websocket]") {
  auto const how = GENERATE(breaks::first_wait, breaks::first_wait_with_no_code);
  breaking_timer context{how};
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};

  auto const failing = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  // Its read stopped, with a code that says the keep-alive failed rather than none.
  REQUIRE(quoll::test::run(context, failing()) == std::errc::io_error);

  auto const after = [&]() -> task<std::error_code> {
    co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "after"));
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(context, after()) == std::errc::io_error);
}

TEST_CASE("websocket: a client's read that throws as the peer is taken as gone throws its own failure",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{breaking_read{std::move(client_end)}}, websocket_role::client, {}, {.ping_interval = 1s}};

  auto const broken = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.receive());
    } catch (std::system_error const &thrown) {
      co_return thrown.code().message();
    } catch (std::runtime_error const &thrown) {
      co_return thrown.what();
    }

    co_return "a message";
  };

  REQUIRE(quoll::test::run(time, broken()) == "the read broke as it was stopped");
  REQUIRE(time.now() == 2s);

  // Receiving has ended, as after any read broken off.
  auto const again = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(time, again()) == std::errc::io_error);
}

TEST_CASE("websocket: a client's keep-alive ends with its receive()", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};

  // The message is there: the receive() ends at once, and its keep-alive with it, so that the run drains then.
  auto const once = [&]() -> task<websocket_message> {
    co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "one"));
    co_return co_await client.receive();
  };

  REQUIRE(quoll::test::run(time, once()) == text("one"));
  REQUIRE(time.now() == 0s);
}

TEST_CASE("websocket: a client's ping whose write fails with a code of zero ends its receive() all the same",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{throwing_on_write{std::move(client_end)}}, websocket_role::client, {}, {.ping_interval = 1s}};

  auto const broken = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(client.receive());
  };

  REQUIRE(quoll::test::run(time, broken()) == std::errc::io_error);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("websocket: a client's keep-alive that fails once its receive() ended changes nothing", "[http][websocket]") {
  breaking_timer context{breaks::when_stopped};
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{io::any_stream{std::move(client_end)}, websocket_role::client, {}, {.ping_interval = 1s}};

  // Each message ends its receive() before the ping is due, which stops its keep-alive's wait: the timer then fails.
  auto const received = [&]() -> task<websocket_message> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(500ms);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "in time"));
      co_await quoll::async::sleep_for(500ms);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "and after"));
    };
    auto const twice = [&]() -> task<websocket_message> {
      auto first = co_await client.receive();

      REQUIRE(first == text("in time"));
      // What the keep-alive threw as it ended was no failure of the connection's.
      co_return co_await client.receive();
    };

    auto [sent, message] = co_await quoll::async::when_all(peer(), twice());

    co_return message;
  };

  REQUIRE(quoll::test::run(context, received()) == text("and after"));
}

TEST_CASE("websocket: a client's keep-alive sends no ping beside one still under way", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{taking_slowly{std::move(client_end), 400ms}}, websocket_role::client, {}, {.ping_interval = 1s}};
  http::websocket_parser from_client{websocket_role::server};

  // The ping of 1 s takes until 3.4 s; the message of 1.5 s ends that receive(), and the next one's keep-alive, due to
  // ping at 2.5 s, finds it still under way: it sends none, and takes the peer as gone at 3.5 s.
  auto const quiet = [&]() -> task<std::error_code> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(1500ms);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "one"));
    };
    auto const twice = [&]() -> task<std::error_code> {
      auto first = co_await client.receive();

      REQUIRE(first == text("one"));
      co_return co_await quoll::test::code_thrown_by(client.receive());
    };

    auto [sent, code] = co_await quoll::async::when_all(peer(), twice());

    co_return code;
  };

  REQUIRE(quoll::test::run(time, quiet()) == std::errc::timed_out);
  REQUIRE(time.now() == 3500ms);

  // One ping on the stream, and nothing after it.
  auto const pings = [&]() -> task<int> {
    auto const first = co_await quoll::test::next_websocket_message(server_end, from_client);

    REQUIRE(first == websocket_message{websocket_opcode::ping, ""});

    auto const second = co_await quoll::test::code_thrown_by(
        quoll::async::with_timeout(10s, quoll::test::next_websocket_message(server_end, from_client)));

    co_return second == std::errc::timed_out ? 1 : 2;
  };

  REQUIRE(quoll::test::run(time, pings()) == 1);
}

TEST_CASE("websocket: a client's ping that fails once its receive() has ended ends the next with its failure",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{reset_late{std::move(client_end), 500ms}}, websocket_role::client, {}, {.ping_interval = 1s}};

  // The ping of 1 s fails at 1.5 s; the message of 1.2 s ended its receive() meanwhile, so that it takes nothing as
  // gone: the next receive()'s keep-alive pings at 2.2 s, and ends it with the reset at 2.7 s, rather than a timeout.
  auto const reset = [&]() -> task<std::error_code> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(1200ms);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "one"));
    };
    auto const twice = [&]() -> task<std::error_code> {
      auto first = co_await client.receive();

      REQUIRE(first == text("one"));
      co_return co_await quoll::test::code_thrown_by(client.receive());
    };

    auto [sent, code] = co_await quoll::async::when_all(peer(), twice());

    co_return code;
  };

  REQUIRE(quoll::test::run(time, reset()) == std::errc::connection_reset);
  REQUIRE(time.now() == 2700ms);
}

TEST_CASE("websocket: a client's keep-alive that counted another's ping as its own gives that ping's failure",
          "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{reset_late{std::move(client_end), 1600ms}}, websocket_role::client, {}, {.ping_interval = 1s}};

  // The ping of 1 s fails at 2.6 s; the message of 1.2 s ended its receive(), and the next one's keep-alive found it
  // under way at 2.2 s and counted it as its own: its verdict at 3.2 s gives the reset rather than a timeout.
  auto const reset = [&]() -> task<std::error_code> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(1200ms);
      co_await send_raw(server_end, http::format_websocket_frame(websocket_opcode::text, "one"));
    };
    auto const twice = [&]() -> task<std::error_code> {
      auto first = co_await client.receive();

      REQUIRE(first == text("one"));
      co_return co_await quoll::test::code_thrown_by(client.receive());
    };

    auto [sent, code] = co_await quoll::async::when_all(peer(), twice());

    co_return code;
  };

  REQUIRE(quoll::test::run(time, reset()) == std::errc::connection_reset);
  REQUIRE(time.now() == 3200ms);
}

TEST_CASE("websocket: a client's keep-alive with sending ended takes the peer as gone for that", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{taking_slowly{std::move(client_end), 400ms}}, websocket_role::client, {}, {.ping_interval = 1s}};
  std::stop_source stop;

  // A send stopped at 0.5 s, after its first byte left at 0.4 s, cuts sending: the ping due at 1 s cannot leave, which
  // is why the peer is taken as gone then, rather than a timeout an interval later.
  auto const cut = [&]() -> task<std::error_code> {
    auto const sending = [&]() -> task<std::error_code> {
      co_return co_await quoll::test::code_thrown_by([&]() -> task<> {
        co_await quoll::async::detail::stop_scope<void>{client.send_text("cut short"), stop.get_token()};
      }());
    };
    auto const stopping = [&]() -> task<> {
      co_await quoll::async::sleep_for(500ms);
      stop.request_stop();
    };

    auto [sent, stopped, code] =
        co_await quoll::async::when_all(sending(), stopping(), quoll::test::code_thrown_by(client.receive()));

    co_return code;
  };

  REQUIRE(quoll::test::run(time, cut()) == http::error::websocket_closed);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("websocket: a client's ping refused as its close left waits for the peer's answer", "[http][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  http::websocket client{
      io::any_stream{taking_slowly{std::move(client_end), 400ms}}, websocket_role::client, {}, {.ping_interval = 1s}};

  // The close takes until 3.2 s to leave, a byte every 400 ms; the ping due at 1 s waits for the turn behind it, then
  // is refused: the peer's answer, at 3.5 s, is waited for as a pong would be, and given.
  auto const closing = [&]() -> task<websocket_message> {
    auto const peer = [&]() -> task<> {
      co_await quoll::async::sleep_for(3500ms);
      co_await send_raw(server_end, http::format_websocket_close(1000));
    };

    auto [closed, answer, answered] = co_await quoll::async::when_all(client.close(), client.receive(), peer());

    co_return answer;
  };

  REQUIRE(quoll::test::run(time, closing()) == close_with(1000));
  REQUIRE(time.now() == 3500ms);
}
