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

#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
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
