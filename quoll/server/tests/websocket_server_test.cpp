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
#include <quoll/async/when_all.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/http/websocket_parser.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::websocket_message;
using http::websocket_opcode;
using http::websocket_role;
using quoll::async::task;

constexpr http::websocket_mask mask{0x0a, 0x0b, 0x0c, 0x0d};

// The handshake of RFC 6455, 1.3, and what follows it.
std::string handshake(std::string const &after = {}, std::string const &path = "/socket") {
  return "GET " + path +
         " HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" +
         after;
}

http::request handshake_request() {
  http::request request;

  request.method = http::verb::get;
  request.target = "/socket";
  request.fields.add("Host", "h");
  request.fields.add("Upgrade", "websocket");
  request.fields.add("Connection", "Upgrade");
  request.fields.add("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  request.fields.add("Sec-WebSocket-Version", "13");
  return request;
}

http::router socket_at(std::function<task<>(http::websocket)> handler) {
  http::router router;

  router.get("/socket", [handler](http::request request) { return http::accept_websocket(request, handler); });
  return router;
}

// The response to what was sent on `client`, and what came after it.
struct answered {
  http::response response;
  std::string after;
};

task<answered> send_then_read(io::memory_stream &client, std::string sent) {
  std::string buffer;
  http::response_parser parser;

  (void)co_await io::write(client, std::as_bytes(std::span{sent}));

  if (auto const failed = co_await http::read(client, buffer, parser)) {
    throw std::system_error{failed};
  }

  co_return answered{parser.release(), std::move(buffer)};
}

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

// What a server with `router` reported, as it served `client`'s exchange.
template <class Client>
std::vector<std::string> served(http::router const &router, Client client, std::stop_token stop_token = {}) {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::vector<std::string> reported;
  http::connection_options options;
  quoll::async::run_loop loop;

  options.on_handler_error = [&reported](std::exception_ptr const &failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &thrown) {
      reported.emplace_back(thrown.what());
    }
  };

  auto const outcome = quoll::test::run(
      loop,
      quoll::async::when_all(http::server{router, {.connection = options}}.serve_connection(std::move(server_end)),
                             client(client_end)),
      std::move(stop_token));

  REQUIRE(std::get<1>(outcome.value()) == 1);
  return reported;
}

} // namespace

TEST_CASE("accept_websocket: a handshake hands the connection over to its handler", "[server][websocket]") {
  auto const router = socket_at(echo);

  auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
    auto [response, after] = co_await send_then_read(client, handshake());

    REQUIRE(response.status == http::status::switching_protocols);
    REQUIRE(response.fields.find("Upgrade") == "websocket");
    REQUIRE(response.fields.find("Connection") == "Upgrade");
    REQUIRE(response.fields.find("Sec-WebSocket-Accept") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    REQUIRE_FALSE(response.fields.contains("Content-Length"));

    http::websocket socket{io::any_stream{std::move(client)}, websocket_role::client, std::move(after)};

    co_await socket.send_text("hello");

    auto const echoed = co_await socket.receive();

    co_await socket.close(1000, "bye");

    auto const closed = co_await socket.receive();

    REQUIRE(echoed == websocket_message{websocket_opcode::text, "hello"});
    REQUIRE(closed == websocket_message{websocket_opcode::close, "", 1000});
    co_return 1;
  });

  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: frames sent right behind the handshake reach the handler", "[server][websocket]") {
  std::optional<websocket_message> first;
  auto const router = socket_at([&first](http::websocket socket) -> task<> { first = co_await socket.receive(); });

  static_cast<void>(served(router, [](io::memory_stream &client) -> task<int> {
    auto const sent = handshake(http::format_websocket_frame(websocket_opcode::text, "early", true, mask));
    auto [response, after] = co_await send_then_read(client, sent);

    REQUIRE(response.status == http::status::switching_protocols);

    // The handler is over, and its connection with it.
    auto const rest = co_await quoll::test::read_until_closed(client);

    REQUIRE(after + rest == "");
    co_return 1;
  }));

  REQUIRE(first == websocket_message{websocket_opcode::text, "early"});
}

TEST_CASE("accept_websocket: a request that is no handshake is refused", "[server][websocket]") {
  auto ran = false;
  auto const handler = [&ran](http::websocket) -> task<> {
    ran = true;
    co_return;
  };

  SECTION("a handshake is accepted") {
    auto const accepted = http::accept_websocket(handshake_request(), handler);

    REQUIRE(accepted.status == http::status::switching_protocols);
    REQUIRE(accepted.upgrade);
  }

  SECTION("a plain request answers 400") {
    auto request = handshake_request();

    request.fields.erase("Upgrade");

    auto const refused = http::accept_websocket(request, handler);

    REQUIRE(refused.status == http::status::bad_request);
    REQUIRE_FALSE(refused.upgrade);
  }

  SECTION("another version answers 426, with the one spoken") {
    auto request = handshake_request();

    request.fields.set("Sec-WebSocket-Version", "8");

    auto const refused = http::accept_websocket(request, handler);

    REQUIRE(refused.status == http::status::upgrade_required);
    REQUIRE(refused.fields.find("Sec-WebSocket-Version") == "13");
    REQUIRE_FALSE(refused.upgrade);
  }

  SECTION("an empty handler is refused") {
    REQUIRE_THROWS_AS(http::accept_websocket(handshake_request(), {}), std::invalid_argument);
  }

  REQUIRE_FALSE(ran);
}

TEST_CASE("serve_connection: a switch that answers no request for one is replaced with a 500", "[server][websocket]") {
  auto handed_over = false;
  auto const taker = [&handed_over](io::any_stream, std::string) -> task<> {
    handed_over = true;
    co_return;
  };
  http::router router;

  router.get("/socket", [&taker](http::request request) {
    auto response = http::accept_websocket(request, [](http::websocket) -> task<> { co_return; });

    response.upgrade = taker;
    return response;
  });
  router.get("/other", [&taker](http::request) {
    http::response response;

    response.status = http::status::switching_protocols;
    response.upgrade = taker;
    return response;
  });
  router.get("/ok", [&taker](http::request) {
    http::response response;

    response.upgrade = taker;
    return response;
  });

  auto const answer_to = [&router](std::string sent) {
    http::status status{};
    auto const reported = served(router, [&sent, &status](io::memory_stream &client) -> task<int> {
      status = (co_await send_then_read(client, sent)).response.status;
      co_return 1;
    });

    REQUIRE(reported.size() == 1);
    return std::pair{status, reported.front()};
  };

  SECTION("a request that did not ask") {
    auto const [status, reported] = answer_to("GET /other HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == "quoll::http::server: a protocol switch the request did not ask for");
  }

  SECTION("a status other than 101") {
    auto const [status, reported] = answer_to(handshake({}, "/ok"));

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == "quoll::http::server: a protocol switch with a status other than 101");
  }

  SECTION("a body beside it") {
    router.use([](http::request request, http::next const &next) -> task<http::response> {
      auto response = co_await next(std::move(request));

      response.body = "more";
      co_return response;
    });

    auto const [status, reported] = answer_to(handshake());

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == "quoll::http::server: a protocol switch with a body");
  }

  REQUIRE_FALSE(handed_over);
}

TEST_CASE("serve_connection: what a WebSocket handler throws is reported, but for a stop", "[server][websocket]") {
  SECTION("a failure") {
    auto const router = socket_at([](http::websocket) -> task<> { throw std::runtime_error{"handler failure"}; });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());

      REQUIRE(answer.response.status == http::status::switching_protocols);
      co_return 1;
    });

    REQUIRE(reported == std::vector<std::string>{"handler failure"});
  }

  SECTION("a stop") {
    std::stop_source stop;
    std::error_code ended;
    auto const router = socket_at([&ended](http::websocket socket) -> task<> {
      try {
        static_cast<void>(co_await socket.receive());
      } catch (std::system_error const &thrown) {
        ended = thrown.code();
        throw;
      }
    });

    auto const reported = served(
        router,
        [&stop](io::memory_stream &client) -> task<int> {
          auto const answer = co_await send_then_read(client, handshake());

          REQUIRE(answer.response.status == http::status::switching_protocols);
          stop.request_stop();
          co_return 1;
        },
        stop.get_token());

    REQUIRE(ended == std::errc::operation_canceled);
    REQUIRE(reported.empty());
  }
}
