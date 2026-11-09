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
#include <quoll/async/task.hpp>
#include <quoll/client/connect_websocket.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/memory_network.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::websocket_message;
using http::websocket_opcode;
using quoll::async::task;
using quoll::test::memory_connector;
using quoll::test::memory_server;
using namespace std::chrono_literals;

// Each message until the close, sent back.
task<> echo(http::websocket socket) {
  for (;;) {
    auto message = co_await socket.receive();

    if (message.opcode == websocket_opcode::close) {
      co_return;
    }

    co_await socket.send_text(message.data);
  }
}

// A server that answers the handshake with `raw`, whatever it asked.
std::function<task<>(io::memory_stream)> answering(std::string raw) {
  return [raw = std::move(raw)](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    static_cast<void>(co_await http::read(stream, buffer, parser));
    (void)co_await io::write(stream, std::as_bytes(std::span{raw}));
  };
}

// What connecting to `server` at `url` threw: its message, empty if nothing.
std::string refusal(memory_server &server, quoll::async::run_loop &loop, std::string url,
                    http::websocket_connect_options options = {}) {
  auto const connecting = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(memory_connector{server}, url, std::move(options)));
    } catch (http::websocket_refused const &refused) {
      co_return std::to_string(static_cast<int>(refused.response().status));
    }

    co_return std::string{};
  };

  return quoll::test::run(loop, connecting()).value();
}

// A connector to a server that never answers: what it is sent is left unread.
class silent_connector {
public:
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    *kept_ = std::move(server_end);
    co_return io::any_stream{std::move(client_end)};
  }

  std::optional<io::memory_stream> *kept_;
};

static_assert(io::Connector<silent_connector>);

} // namespace

TEST_CASE("connect_websocket: opens a WebSocket to a server, which answers what it sends", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string target;
  http::router router;

  router.get("/room", [&target](http::request request) {
    target = request.target;
    return http::accept_websocket(request, echo);
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  auto const talking = [&]() -> task<int> {
    auto [socket, handshake] =
        co_await http::connect_websocket(memory_connector{server}, "ws://chat.example/room?id=4");

    co_await socket.send_text("hello");

    auto const echoed = co_await socket.receive();

    co_await socket.close();

    auto const closed = co_await socket.receive();

    REQUIRE(handshake.status == http::status::switching_protocols);
    REQUIRE(echoed == websocket_message{websocket_opcode::text, "hello"});
    REQUIRE(closed == websocket_message{websocket_opcode::close, "", 1000});
    co_return 1;
  };

  // The server takes only masked frames: what it echoed came from a client's end.
  REQUIRE(quoll::test::run(loop, talking()) == 1);
  REQUIRE(target == "/room?id=4");
  REQUIRE(server.destination == "chat.example:80");
}

TEST_CASE("connect_websocket: a subprotocol chosen is one offered", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string chosen;
  http::router router;

  router.get("/", [&chosen](http::request request) {
    auto response = http::accept_websocket(request, echo);

    response.fields.set("Sec-WebSocket-Protocol", chosen);
    return response;
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};
  http::websocket_connect_options options;

  options.fields.set("Sec-WebSocket-Protocol", "chat, superchat");

  SECTION("one offered") {
    chosen = "superchat";

    auto const connecting = [&]() -> task<std::string> {
      auto connected = co_await http::connect_websocket(memory_connector{server}, "ws://h/", options);

      co_await connected.socket.close();
      static_cast<void>(co_await connected.socket.receive());
      co_return std::string{connected.handshake.fields.find("Sec-WebSocket-Protocol").value_or("")};
    };

    REQUIRE(quoll::test::run(loop, connecting()) == "superchat");
  }

  SECTION("one not offered is refused") {
    chosen = "other";

    REQUIRE(refusal(server, loop, "ws://h/", options) == "101");
  }
}

TEST_CASE("connect_websocket: what is no switch to the WebSocket asked for is refused, with its answer",
          "[client][websocket]") {
  quoll::async::run_loop loop;

  SECTION("another status") {
    http::router router;

    router.get("/", [](http::request) {
      http::response response;

      response.status = http::status::forbidden;
      return response;
    });

    memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "403");
  }

  SECTION("a 101 with another accept") {
    memory_server server{loop.get_executor(),
                         answering("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
                         0,
                         {},
                         {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "101");
  }

  SECTION("a 101 with an extension none offered") {
    http::router router;

    router.get("/", [](http::request request) {
      auto response = http::accept_websocket(request, echo);

      response.fields.set("Sec-WebSocket-Extensions", "permessage-deflate");
      return response;
    });

    memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "101");
  }
}

TEST_CASE("connect_websocket: what cannot be asked is refused when it is called", "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(), answering(""), 0, {}, {}};
  http::websocket_connect_options reserved;

  reserved.fields.set("Sec-WebSocket-Key", "mine");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "http://h/"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/#part"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/?a b"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", reserved), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "wss://h/"), std::invalid_argument);
}

TEST_CASE("connect_websocket: a server that does not answer the handshake in time", "[client][websocket]") {
  quoll::test::virtual_time time;
  std::optional<io::memory_stream> kept;
  http::websocket_connect_options options;

  options.handshake_timeout = 1s;

  auto const connecting = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(
        http::connect_websocket(silent_connector{&kept}, "ws://h/", std::move(options)));
  };

  REQUIRE(quoll::test::run(time, connecting()) == std::errc::timed_out);
  REQUIRE(time.now() == 1s);
}
