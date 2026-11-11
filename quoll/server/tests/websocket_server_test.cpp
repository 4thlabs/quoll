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
#include <quoll/async/current_executor.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/http/next.hpp>
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
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/next_websocket_message.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/reports.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/test/websocket_echo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
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
using namespace std::chrono_literals;

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

http::router socket_at(std::function<task<>(http::websocket)> handler, http::websocket_options options = {}) {
  http::router router;

  router.get("/socket",
             [handler, options](http::request request) { return http::accept_websocket(request, handler, options); });
  return router;
}

// The response to what was sent on `client`, and what came after it.
struct answered {
  http::response response;
  std::string after;
};

task<answered> send_then_read(io::memory_stream &client, std::string sent, http::verb method = http::verb::get) {
  std::string buffer;
  http::response_parser parser;

  parser.set_request_method(method);

  (void)co_await io::write(client, std::as_bytes(std::span{sent}));

  if (auto const failed = co_await http::read(client, buffer, parser)) {
    throw std::system_error{failed};
  }

  co_return answered{parser.release(), std::move(buffer)};
}

// What a server with `router` and `options` reported, as it served `client`'s exchange on `context`.
template <class Context, class Client>
std::vector<std::string> served_on(Context &context, http::router const &router, Client client,
                                   http::connection_options options = {}, std::stop_token stop_token = {}) {
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::vector<std::string> reported;

  options.on_handler_error = [&reported](std::exception_ptr const &failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &thrown) {
      reported.emplace_back(thrown.what());
    }
  };

  auto const outcome = quoll::test::run(
      context,
      quoll::async::when_all(http::server{router, {.connection = options}}.serve_connection(std::move(server_end)),
                             client(client_end)),
      std::move(stop_token));

  REQUIRE(std::get<1>(outcome.value()) == 1);
  return reported;
}

template <class Client>
std::vector<std::string> served(http::router const &router, Client client, std::stop_token stop_token = {}) {
  quoll::async::run_loop loop;

  return served_on(loop, router, std::move(client), {}, std::move(stop_token));
}

// Each message until the close, recording how receiving ended.
// `work`, saying in `ended` how it ended: its failure's code, or operation_canceled for a stop, which it ends on all
// the same, as the handler would.
task<> telling(task<> work, std::error_code &ended) {
  try {
    if (co_await quoll::async::stopped_as_optional(std::move(work))) {
      co_return;
    }
  } catch (std::system_error const &thrown) {
    ended = thrown.code();
    throw;
  }

  ended = std::make_error_code(std::errc::operation_canceled);
  co_await quoll::async::detail::end_stopped{};
}

task<> receive_until_closed(http::websocket socket) {
  for (;;) {
    if ((co_await socket.receive()).opcode == websocket_opcode::close) {
      co_return;
    }
  }
}

task<> receive_one(http::websocket socket) { static_cast<void>(co_await socket.receive()); }

task<> ticking(http::websocket socket) {
  using namespace std::chrono_literals;

  for (;;) {
    co_await socket.send_text("tick");
    co_await quoll::async::sleep_for(500ms);
  }
}

task<> closing_then_receiving(http::websocket socket) {
  co_await socket.close();
  static_cast<void>(co_await socket.receive());
}

task<> receive_all(http::websocket socket, std::error_code &ended) {
  co_await telling(receive_until_closed(std::move(socket)), ended);
}

} // namespace

TEST_CASE("accept_websocket: a handshake hands the connection over to its handler", "[server][websocket]") {
  auto const router = socket_at(quoll::test::websocket_echo);

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

    // The handler is over, and its connection with it, behind the close it did not send.
    auto const rest = co_await quoll::test::read_until_closed(client);
    http::websocket_parser from_server{websocket_role::client};

    from_server.feed(after + rest);

    REQUIRE(from_server.next() == websocket_message{websocket_opcode::close, "", 1000});
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
    REQUIRE(refused.fields.find("Upgrade") == "websocket");
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
  auto const taker = [&handed_over](io::any_stream, std::string, std::stop_token) -> task<> {
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
  router.get("/writer", [&taker](http::request) {
    http::response response;

    response.status = http::status::switching_protocols;
    response.body_writer = [](http::body_stream) -> task<> { co_return; };
    response.upgrade = taker;
    return response;
  });
  router.get("/late", [&taker](http::request) {
    http::response response;

    response.status = http::status::switching_protocols;
    response.late_head_writer = [](http::body_stream) -> task<> { co_return; };
    response.upgrade = taker;
    return response;
  });
  router.post("/streamed", [&taker](http::body_reader) -> task<http::response> {
    http::response response;

    response.status = http::status::switching_protocols;
    response.upgrade = taker;
    co_return response;
  });

  auto const answer_to = [&router](std::string sent, http::verb method = http::verb::get) {
    http::status status{};
    auto const reported = served(router, [&sent, &status, method](io::memory_stream &client) -> task<int> {
      status = (co_await send_then_read(client, sent, method)).response.status;
      co_return 1;
    });

    REQUIRE(reported.size() == 1);
    return std::pair{status, reported.front()};
  };
  auto const did_not_ask = std::string{"quoll::http::server: a protocol switch the request did not ask for"};
  auto const with_a_body = std::string{"quoll::http::server: a protocol switch with a body"};
  auto const asking = std::string{"Upgrade: websocket\r\nConnection: Upgrade\r\n"};

  SECTION("a request that did not ask") {
    auto const [status, reported] = answer_to("GET /other HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == did_not_ask);
  }

  SECTION("CONNECT, which names no protocol") {
    router.use([&taker](http::request, http::next const &) -> task<http::response> {
      http::response response;

      response.status = http::status::switching_protocols;
      response.upgrade = taker;
      co_return response;
    });

    auto const [status, reported] = answer_to("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == did_not_ask);
  }

  SECTION("HEAD") {
    auto const [status, reported] =
        answer_to("HEAD /other HTTP/1.1\r\nHost: h\r\n" + asking + "\r\n", http::verb::head);

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == did_not_ask);
  }

  SECTION("HTTP/1.0") {
    auto const [status, reported] = answer_to("GET /other HTTP/1.0\r\nHost: h\r\n" + asking + "\r\n");

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == did_not_ask);
  }

  SECTION("a request whose body is still coming") {
    auto const [status, reported] =
        answer_to("POST /streamed HTTP/1.1\r\nHost: h\r\n" + asking + "Content-Length: 4\r\n\r\nbody");

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == did_not_ask);
  }

  SECTION("a body_writer beside it") {
    auto const [status, reported] = answer_to(handshake({}, "/writer"));

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == with_a_body);
  }

  SECTION("a late_head_writer beside it") {
    auto const [status, reported] = answer_to(handshake({}, "/late"));

    REQUIRE(status == http::status::internal_server_error);
    REQUIRE(reported == with_a_body);
  }

  SECTION("a head the serializer refuses") {
    router.use([](http::request request, http::next const &next) -> task<http::response> {
      auto response = co_await next(std::move(request));

      response.fields.set("Content-Length", "0");
      co_return response;
    });

    auto const [status, reported] = answer_to(handshake());

    // Its 500 alone, on a connection that is not handed over.
    REQUIRE(status == http::status::internal_server_error);
    REQUIRE_FALSE(reported.empty());
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
    REQUIRE(reported == with_a_body);
  }

  REQUIRE_FALSE(handed_over);
}

TEST_CASE("serve_connection: what a WebSocket handler throws is reported, but for what a stop throws",
          "[server][websocket]") {
  SECTION("a failure") {
    auto const router = socket_at([](http::websocket) -> task<> { throw std::runtime_error{"handler failure"}; });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto answer = co_await send_then_read(client, handshake());
      http::websocket_parser from_server{websocket_role::client};

      from_server.feed(answer.after);

      auto const close = co_await quoll::test::next_websocket_message(client, from_server);

      REQUIRE(answer.response.status == http::status::switching_protocols);
      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1011});
      co_return 1;
    });

    REQUIRE(reported == std::vector<std::string>{"handler failure"});
  }

  SECTION("with no callback, written out, naming a connection taken over rather than a 500") {
    auto const router = socket_at([](http::websocket) -> task<> { throw std::runtime_error{"handler failure"}; });
    auto [client_end, server_end] = io::make_memory_stream_pair();
    auto const talking = [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());

      REQUIRE(answer.response.status == http::status::switching_protocols);
      co_return 1;
    };
    auto const outcome = quoll::test::run(
        quoll::async::when_all(http::server{router}.serve_connection(std::move(server_end)), talking(client_end)));

    REQUIRE(std::get<1>(outcome.value()) == 1);
    REQUIRE(quoll::test::reports() ==
            std::vector<std::string>{
                "quoll: the handler of a connection taken over, as a WebSocket, threw: handler failure\n"});
  }

  SECTION("with no callback, what its peer causes is not written out") {
    // Reading until the peer goes, which it does with no close: the end of the stream, which a peer causes at will.
    auto const router = socket_at([](http::websocket socket) -> task<> {
      for (;;) {
        static_cast<void>(co_await socket.receive());
      }
    });
    auto [client_end, server_end] = io::make_memory_stream_pair();
    auto const leaving = [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());

      REQUIRE(answer.response.status == http::status::switching_protocols);
      client.close();
      co_return 1;
    };
    auto const outcome = quoll::test::run(
        quoll::async::when_all(http::server{router}.serve_connection(std::move(server_end)), leaving(client_end)));

    REQUIRE(std::get<1>(outcome.value()) == 1);
    REQUIRE(quoll::test::reports().empty());
  }

  SECTION("a send refused after the handler's own close") {
    auto const router = socket_at([](http::websocket socket) -> task<> {
      co_await socket.close();
      co_await socket.send_text("late");
    });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());

      REQUIRE(answer.response.status == http::status::switching_protocols);
      co_return 1;
    });

    // A send refused after a close is how the connection ends, whoever closed.
    REQUIRE(reported.empty());
  }

  SECTION("a copy left receiving past the handler") {
    auto const router = socket_at([](http::websocket socket) -> task<> {
      auto const left = [](http::websocket copy) -> task<> {
        while ((co_await copy.receive()).opcode != websocket_opcode::close) {
        }
      };

      quoll::async::co_spawn(co_await quoll::async::current_executor(), left(socket), quoll::async::detached);

      // The copy receiving before the handler ends.
      co_await quoll::async::yield();
    });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto answer = co_await send_then_read(client, handshake());
      http::websocket_parser from_server{websocket_role::client};

      from_server.feed(answer.after);

      // The server's close as the handler ends, answered to the copy, which ends the connection.
      auto const close = co_await quoll::test::next_websocket_message(client, from_server);
      auto const answer_close = http::format_websocket_close(1000, "", mask);

      (void)co_await io::write(client, std::as_bytes(std::span{answer_close}));

      auto const rest = co_await quoll::test::read_until_closed(client);

      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1000});
      REQUIRE(rest.empty());
      co_return 1;
    });

    REQUIRE(reported.empty());
  }

  SECTION("a send refused after the peer's close, which could not be answered") {
    std::error_code refused;
    auto const router = socket_at([&refused](http::websocket socket) -> task<> {
      static_cast<void>(co_await socket.receive());

      try {
        co_await socket.send_text("late");
      } catch (std::system_error const &thrown) {
        refused = thrown.code();
        throw;
      }
    });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());
      auto const close = http::format_websocket_close(1000, "", mask);

      REQUIRE(answer.response.status == http::status::switching_protocols);

      // Gone with its close, as some peers are: the answer finds no one.
      (void)co_await io::write(client, std::as_bytes(std::span{close}));

      { auto const gone = std::move(client); }
      co_return 1;
    });

    REQUIRE(refused == http::error::websocket_closed);
    REQUIRE(reported.empty());
  }

  SECTION("a peer gone") {
    std::error_code ended;
    auto const router = socket_at([&ended](http::websocket socket) { return receive_all(std::move(socket), ended); });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto const answer = co_await send_then_read(client, handshake());

      REQUIRE(answer.response.status == http::status::switching_protocols);

      { auto const gone = std::move(client); }
      co_return 1;
    });

    REQUIRE(ended == io::error::end_of_stream);
    // What receive() threw, let through.
    REQUIRE(reported.size() == 1);
  }

  SECTION("a peer breaking the protocol") {
    std::error_code ended;
    auto const router = socket_at([&ended](http::websocket socket) { return receive_all(std::move(socket), ended); });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      // Not masked.
      auto const sent = handshake(http::format_websocket_frame(websocket_opcode::text, "plain"));
      auto answer = co_await send_then_read(client, sent);
      http::websocket_parser from_server{websocket_role::client};

      from_server.feed(answer.after);

      auto const close = co_await quoll::test::next_websocket_message(client, from_server);

      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1002});
      co_return 1;
    });

    REQUIRE(ended == http::error::websocket_protocol);
    // What receive() threw, let through.
    REQUIRE(reported.size() == 1);
  }

  SECTION("operation_canceled with no stop asked for") {
    auto const router = socket_at([](http::websocket) -> task<> {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "its own"};
    });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto answer = co_await send_then_read(client, handshake());
      http::websocket_parser from_server{websocket_role::client};

      from_server.feed(answer.after);

      auto const close = co_await quoll::test::next_websocket_message(client, from_server);

      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1011});
      co_return 1;
    });

    REQUIRE(reported.size() == 1);
  }

  SECTION("a stop") {
    std::stop_source stop;
    std::error_code ended;
    auto const router =
        socket_at([&ended](http::websocket socket) { return telling(receive_one(std::move(socket)), ended); });

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

TEST_CASE("accept_websocket: a quiet peer is pinged, then its handler stopped once it has gone",
          "[server][websocket]") {
  quoll::test::virtual_time time;
  std::error_code ended;
  auto const router = socket_at([&ended](http::websocket socket) { return receive_all(std::move(socket), ended); },
                                {.ping_interval = 1s});

  auto const reported = served_on(time, router, [&time](io::memory_stream &client) -> task<int> {
    auto answer = co_await send_then_read(client, handshake());
    http::websocket_parser from_server{websocket_role::client};

    from_server.feed(answer.after);

    // Quiet since the switch.
    auto const first = co_await quoll::test::next_websocket_message(client, from_server);
    auto const first_at = time.now();
    auto const pong = http::format_websocket_frame(websocket_opcode::pong, "", true, mask);

    (void)co_await io::write(client, std::as_bytes(std::span{pong}));

    // The pong counted: not pinged at the next tick, but at the one after.
    auto const second = co_await quoll::test::next_websocket_message(client, from_server);
    auto const second_at = time.now();
    // No answer: gone at the next tick, sent a close as the handler ends, and the connection with it.
    auto const rest = co_await quoll::test::read_until_closed(client);
    http::websocket_parser closing{websocket_role::client};

    closing.feed(rest);

    REQUIRE(first.opcode == websocket_opcode::ping);
    REQUIRE(first_at == 1s);
    REQUIRE(second.opcode == websocket_opcode::ping);
    REQUIRE(second_at == 3s);
    REQUIRE(closing.next() == websocket_message{websocket_opcode::close, "", 1000});
    REQUIRE(time.now() == 4s);
    co_return 1;
  });

  REQUIRE(ended == std::errc::operation_canceled);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a drain sends a close with 1001, which the handler sees answered", "[server][websocket]") {
  quoll::test::virtual_time time;
  std::stop_source drain;
  http::connection_options options;
  std::error_code ended;
  auto const router = socket_at([&ended](http::websocket socket) { return receive_all(std::move(socket), ended); });

  options.drain = drain.get_token();

  auto const reported = served_on(
      time, router,
      [&drain](io::memory_stream &client) -> task<int> {
        auto answer = co_await send_then_read(client, handshake());
        http::websocket_parser from_server{websocket_role::client};

        from_server.feed(answer.after);
        drain.request_stop();

        auto const close = co_await quoll::test::next_websocket_message(client, from_server);
        auto const answer_close = http::format_websocket_close(1001, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{answer_close}));

        auto const rest = co_await quoll::test::read_until_closed(client);

        REQUIRE(close == websocket_message{websocket_opcode::close, "", 1001});
        REQUIRE(rest.empty());
        co_return 1;
      },
      options);

  // The handler saw the close and returned.
  REQUIRE_FALSE(ended);
  REQUIRE(reported.empty());
  REQUIRE(time.now() == 0s);
}

namespace {

// A server's stream whose writes take a byte each `per_byte`, once it is set, a stop ending one with nothing written:
// a peer slow to read, or one that no longer reads, with a day per byte.
class paced_writes {
public:
  paced_writes(io::memory_stream inner, std::chrono::nanoseconds const &per_byte) noexcept
      : inner_(std::move(inner)), per_byte_(&per_byte) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (*per_byte_ <= std::chrono::nanoseconds::zero()) {
      co_return co_await inner_.write_some(data);
    }

    // Stopped, it ends on the stop, as a stream's does.
    co_await quoll::async::sleep_for(*per_byte_);

    co_return co_await inner_.write_some(data.first(1));
  }

  [[nodiscard]] task<io::io_result> shutdown_send() {
    if (*per_byte_ == stalled_shutdown) {
      // Stopped, it ends on the stop, as a stream's does.
      co_await quoll::async::sleep_for(24h);
    }

    co_return co_await inner_.shutdown_send();
  }

  // As a pace: writes at once, and a shutdown that the peer never takes, as TLS's closing message to a peer that no
  // longer reads.
  static constexpr std::chrono::nanoseconds stalled_shutdown{-1};

private:
  io::memory_stream inner_;
  std::chrono::nanoseconds const *per_byte_;
};

constexpr std::chrono::nanoseconds no_reading = 24h;

struct paced_run {
  std::chrono::nanoseconds ended_at;
  std::vector<std::string> reported;
};

// A connection run on virtual time over a paced_writes stream, its handler given the pace to set, with `options`:
// when it ended, and what the server reported. The client sends the handshake, then does what `client` says with the
// rest of what came, by default reading to the end.
template <class Handler, class Client>
paced_run run_paced(Handler handler, http::websocket_options options, Client client, std::stop_token stop = {},
                    std::stop_token drain = {}) {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::chrono::nanoseconds per_byte{};
  std::vector<std::string> reported;
  http::connection_options connection;
  auto const router = socket_at(
      [&per_byte, &handler](http::websocket socket) { return handler(std::move(socket), per_byte); }, options);

  connection.drain = std::move(drain);

  connection.on_handler_error = [&reported](std::exception_ptr const &failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &thrown) {
      reported.emplace_back(thrown.what());
    }
  };

  auto const talk = [&]() -> task<int> {
    auto answer = co_await send_then_read(client_end, handshake());

    REQUIRE(answer.response.status == http::status::switching_protocols);
    co_return co_await client(client_end, std::move(answer.after));
  };

  auto const outcome =
      quoll::test::run(time,
                       quoll::async::when_all(http::server{router, {.connection = connection}}.serve_connection(
                                                  paced_writes{std::move(server_end), per_byte}),
                                              talk()),
                       std::move(stop));

  REQUIRE(std::get<1>(outcome.value()) == 1);
  return {time.now(), reported};
}

// What a client does that only reads to the end.
task<int> read_to_the_end(io::memory_stream &client, std::string) {
  static_cast<void>(co_await quoll::test::read_until_closed(client));
  co_return 1;
}

// What a client does that reads `expected` from the server, answers its close, then reads to the end.
auto expecting(std::vector<websocket_message> expected) {
  return [expected = std::move(expected)](io::memory_stream &client, std::string after) -> task<int> {
    http::websocket_parser from_server{websocket_role::client};
    std::vector<websocket_message> received;

    from_server.feed(after);

    while (received.size() < expected.size()) {
      received.push_back(co_await quoll::test::next_websocket_message(client, from_server));
    }

    auto const answer = http::format_websocket_close(1000, "", mask);

    (void)co_await io::write(client, std::as_bytes(std::span{answer}));
    static_cast<void>(co_await quoll::test::read_until_closed(client));
    REQUIRE(received == expected);
    co_return 1;
  };
}

} // namespace

TEST_CASE("accept_websocket: a peer that takes nothing has its handler stopped", "[server][websocket]") {
  std::error_code ended;

  auto const [ended_at, reported] = run_paced(
      [&ended](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = no_reading;

        try {
          co_await socket.send_text("never taken");
        } catch (std::system_error const &thrown) {
          ended = thrown.code();
          throw;
        }
      },
      {.ping_interval = 1s}, read_to_the_end);

  // Its message waits a second with nothing taken: gone then, the send throwing, which the handler let through, and
  // nothing more is written to it.
  REQUIRE(ended_at == 1s);
  REQUIRE(ended == std::errc::timed_out);
  REQUIRE(reported.size() == 1);
}

TEST_CASE("accept_websocket: a ping the peer does not take, as the handler ends, is no failure",
          "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = no_reading;
        co_await quoll::async::sleep_for(1500ms);
      },
      {.ping_interval = 1s}, read_to_the_end);

  // The ping, sent at 1 s, goes on as the handler ends at 1.5 s, and is not taken by 2 s: nothing more is written.
  REQUIRE(ended_at == 2s);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a ping under way as the handler ends goes whole, then the close", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = 100ms;
        co_await quoll::async::sleep_for(1050ms);
      },
      {.ping_interval = 1s},
      expecting({websocket_message{websocket_opcode::ping, ""}, websocket_message{websocket_opcode::close, "", 1000}}));

  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a live peer slowly taking a long message is not taken as gone", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        // A byte every 0.9 s: six of them, longer than a ping and its wait. Its close then goes at once.
        per_byte = 900ms;
        co_await socket.send_text("slow");
        per_byte = 0s;
      },
      {.ping_interval = 1s},
      expecting(
          {websocket_message{websocket_opcode::text, "slow"}, websocket_message{websocket_opcode::close, "", 1000}}));

  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: with no ping, a close still waits 30 s at most", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = no_reading;
        co_return;
      },
      {.ping_interval = 0s}, read_to_the_end);

  REQUIRE(ended_at == 30s);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a peer that answers nothing is gone, whatever the handler sends", "[server][websocket]") {
  std::error_code ended;

  auto const [ended_at, reported] =
      run_paced([&ended](http::websocket socket,
                         std::chrono::nanoseconds &) { return telling(ticking(std::move(socket)), ended); },
                {.ping_interval = 1s}, read_to_the_end);

  // Its sends tell nothing of the peer: pinged at 1 s, gone at 2 s.
  REQUIRE(ended_at == 2s);
  REQUIRE(ended == std::errc::operation_canceled);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a peer that answers each ping stays", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &) -> task<> {
        while ((co_await socket.receive()).opcode != websocket_opcode::close) {
        }
      },
      {.ping_interval = 1s},
      [](io::memory_stream &client, std::string after) -> task<int> {
        http::websocket_parser from_server{websocket_role::client};
        auto pings = 0;

        from_server.feed(after);

        // Five pings answered, over more than twice the time a silent peer is given.
        while (pings < 5) {
          auto const message = co_await quoll::test::next_websocket_message(client, from_server);

          REQUIRE(message.opcode == websocket_opcode::ping);
          ++pings;

          auto const pong = http::format_websocket_frame(websocket_opcode::pong, message.data, true, mask);

          (void)co_await io::write(client, std::as_bytes(std::span{pong}));
        }

        auto const close = http::format_websocket_close(1000, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{close}));

        auto const answer = co_await quoll::test::next_websocket_message(client, from_server);

        REQUIRE(answer == websocket_message{websocket_opcode::close, "", 1000});
        co_return 1;
      });

  // Pinged at 1, 3, 5, 7 and 9 s: each pong counts, then a quiet second, then the next ping.
  REQUIRE(ended_at == 9s);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a ping under way as the handler throws goes whole, then a 1011", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = 100ms;
        co_await quoll::async::sleep_for(1050ms);
        throw std::runtime_error{"handler failure"};
      },
      {.ping_interval = 1s},
      expecting({websocket_message{websocket_opcode::ping, ""}, websocket_message{websocket_opcode::close, "", 1011}}));

  REQUIRE(reported == std::vector<std::string>{"handler failure"});
}

TEST_CASE("accept_websocket: a pong goes whole, whatever stops the receive() that read the ping",
          "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = 100ms;

        // Stopped as the pong is being written, two bytes in 0.2 s.
        auto const waited = co_await quoll::test::code_thrown_by(quoll::async::with_timeout(50ms, socket.receive()));

        REQUIRE(waited == std::errc::timed_out);
        co_await socket.send_text("after");
      },
      {.ping_interval = 1s},
      [](io::memory_stream &client, std::string after) -> task<int> {
        auto const ping = http::format_websocket_frame(websocket_opcode::ping, "", true, mask);

        // Named, and awaited while it lives: its captures are not in the frame of what it returns.
        auto const rest = expecting({websocket_message{websocket_opcode::pong, ""},
                                     websocket_message{websocket_opcode::text, "after"},
                                     websocket_message{websocket_opcode::close, "", 1000}});

        (void)co_await io::write(client, std::as_bytes(std::span{ping}));

        auto const done = co_await rest(client, std::move(after));

        co_return done;
      });

  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a send after a frame the handler's own stop cut is its failure", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        // Stopped once a byte has left.
        per_byte = 1s;

        auto const cut =
            co_await quoll::test::code_thrown_by(quoll::async::with_timeout(1500ms, socket.send_text("cut short")));

        REQUIRE(cut == std::errc::timed_out);
        co_await socket.send_text("next");
      },
      {.ping_interval = 0s}, read_to_the_end);

  // Refused after a frame cut short, not a close: the handler's failure, reported.
  REQUIRE(reported == std::vector<std::string>{"quoll::http::websocket: cannot send: WebSocket connection closed"});
}

TEST_CASE("accept_websocket: the keep-alive watches nothing once the peer has closed", "[server][websocket]") {
  auto done = false;

  auto const [ended_at, reported] = run_paced(
      [&done](http::websocket socket, std::chrono::nanoseconds &) -> task<> {
        while ((co_await socket.receive()).opcode != websocket_opcode::close) {
        }

        // Its own work after the close, longer than a silent peer is given.
        co_await quoll::async::sleep_for(5s);
        done = true;
      },
      {.ping_interval = 1s},
      [](io::memory_stream &client, std::string) -> task<int> {
        auto const close = http::format_websocket_close(1000, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{close}));
        static_cast<void>(co_await quoll::test::read_until_closed(client));
        co_return 1;
      });

  REQUIRE(done);
  REQUIRE(ended_at == 5s);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a send stopped before anything of it left leaves sending open", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = 1s;

        auto const stopped =
            co_await quoll::test::code_thrown_by(quoll::async::with_timeout(500ms, socket.send_text("not sent")));

        REQUIRE(stopped == std::errc::timed_out);
        per_byte = 0s;
        co_await socket.send_text("sent");
      },
      {.ping_interval = 0s},
      expecting(
          {websocket_message{websocket_opcode::text, "sent"}, websocket_message{websocket_opcode::close, "", 1000}}));

  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a peer that does not answer the handler's close has it stopped", "[server][websocket]") {
  std::error_code ended;

  auto const [ended_at, reported] = run_paced(
      [&ended](http::websocket socket, std::chrono::nanoseconds &) {
        return telling(closing_then_receiving(std::move(socket)), ended);
      },
      {.ping_interval = 1s}, read_to_the_end);

  // Its close's answer waited for as a pong is: nothing read by 1 s, gone.
  REQUIRE(ended_at == 1s);
  REQUIRE(ended == std::errc::operation_canceled);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a keep-alive frame stuck on its peer ends with the connection's stop",
          "[server][websocket]") {
  std::stop_source stop;

  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = no_reading;
        static_cast<void>(co_await socket.receive());
      },
      {.ping_interval = 1s},
      [&stop](io::memory_stream &, std::string) -> task<int> {
        // The ping, at 1 s, waits for a peer that takes nothing, until 2 s but for the stop.
        co_await quoll::async::sleep_for(1500ms);
        stop.request_stop();
        co_return 1;
      },
      stop.get_token());

  REQUIRE(ended_at == 1500ms);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a receive() after one its own timeout ended throws that end, without reading",
          "[server][websocket][stopped]") {
  std::string first;
  std::string again;

  auto const [ended_at, reported] = run_paced(
      [&first, &again](http::websocket socket, std::chrono::nanoseconds &) -> task<> {
        // Stopped by its own timeout, which a stop ends here: as a stream does not take a read up again, receiving
        // ends.
        first = (co_await quoll::test::code_thrown_by(quoll::async::with_timeout(1s, socket.receive()))).message();

        // With no stop asked of it, it throws what ended receiving, rather than read again.
        try {
          static_cast<void>(co_await socket.receive());
        } catch (std::system_error const &thrown) {
          again = thrown.what();
        }
      },
      {.ping_interval = 0s}, read_to_the_end);

  REQUIRE(ended_at == 1s);
  REQUIRE(first == std::make_error_code(std::errc::timed_out).message());
  REQUIRE(again.starts_with("quoll::http::websocket: receiving has ended"));
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a shutdown that does not leave holds no receive()", "[server][websocket]") {
  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        per_byte = paced_writes::stalled_shutdown;
        static_cast<void>(co_await socket.receive());
      },
      {.ping_interval = 1s},
      [](io::memory_stream &client, std::string) -> task<int> {
        auto const close = http::format_websocket_close(1000, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{close}));
        static_cast<void>(co_await quoll::test::read_until_closed(client));
        co_return 1;
      });

  // Given up by the receive() at 1 s, tried again by the handler's end, given up at 2 s.
  REQUIRE(ended_at == 2s);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a drain after the handler's close changes nothing to the wait", "[server][websocket]") {
  std::stop_source drain;

  auto const [ended_at, reported] = run_paced(
      [](http::websocket socket, std::chrono::nanoseconds &) -> task<> {
        co_await socket.close();
        static_cast<void>(co_await socket.receive());
      },
      {.ping_interval = 1s},
      [&drain](io::memory_stream &client, std::string) -> task<int> {
        co_await quoll::async::sleep_for(500ms);
        drain.request_stop();
        static_cast<void>(co_await quoll::test::read_until_closed(client));
        co_return 1;
      },
      {}, drain.get_token());

  // The drain wakes it at 0.5 s, without a spin, and the wait for the close's answer starts again: gone at 1.5 s.
  REQUIRE(ended_at == 1500ms);
  REQUIRE(reported.empty());
}

TEST_CASE(
    "accept_websocket: the peer's close is answered at the handler's end when the receive() that read it is stopped",
    "[server][websocket]") {
  auto answered = false;

  auto const [ended_at, reported] = run_paced(
      [&answered](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        auto const sending = [&socket]() -> task<> { co_await socket.send_text("a long message, 2 s"); };
        auto const receiving = [&socket, &answered]() -> task<> {
          // Stopped at 0.1 s, as its answer waits behind the long message: the close at the handler's end answers.
          auto const close = co_await quoll::async::with_timeout(100ms, socket.receive());

          answered = close == websocket_message{websocket_opcode::close, "", 4000};
        };

        per_byte = 100ms;
        static_cast<void>(co_await quoll::async::when_all(sending(), receiving()));
      },
      {.ping_interval = 0s},
      [](io::memory_stream &client, std::string after) -> task<int> {
        auto const rest = expecting({websocket_message{websocket_opcode::text, "a long message, 2 s"},
                                     websocket_message{websocket_opcode::close, "", 1000}});
        auto const close = http::format_websocket_close(4000, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{close}));

        auto const done = co_await rest(client, std::move(after));

        co_return done;
      });

  REQUIRE(answered);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a send stuck on a peer that closed holds neither the turn nor the receive()",
          "[server][websocket]") {
  auto received = false;

  auto const [ended_at, reported] = run_paced(
      [&received](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        auto const sending = [&socket]() -> task<> {
          static_cast<void>(co_await quoll::test::code_thrown_by(socket.send_text("never taken")));
        };
        auto const receiving = [&socket, &received]() -> task<> {
          received = (co_await socket.receive()) == websocket_message{websocket_opcode::close, "", 1000};
        };

        per_byte = no_reading;
        static_cast<void>(co_await quoll::async::when_all(sending(), receiving()));
      },
      {.ping_interval = 0s},
      [](io::memory_stream &client, std::string) -> task<int> {
        auto const close = http::format_websocket_close(1000, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{close}));
        static_cast<void>(co_await quoll::test::read_until_closed(client));
        co_return 1;
      });

  // The send gives up after 30 s with nothing taken, and the close's answer, behind it, is then no more to send.
  REQUIRE(ended_at == 30s);
  REQUIRE(received);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a drain ends a handler that sends with no failure reported", "[server][websocket]") {
  std::stop_source drain;
  std::error_code refused;

  auto const [ended_at, reported] = run_paced(
      [&refused](http::websocket socket, std::chrono::nanoseconds &) -> task<> {
        auto const sending = [&socket, &refused]() -> task<> {
          try {
            for (;;) {
              co_await socket.send_text("tick");
              co_await quoll::async::sleep_for(300ms);
            }
          } catch (std::system_error const &thrown) {
            refused = thrown.code();
            throw;
          }
        };
        auto const receiving = [&socket]() -> task<> {
          while ((co_await socket.receive()).opcode != websocket_opcode::close) {
          }
        };

        static_cast<void>(co_await quoll::async::when_all(sending(), receiving()));
      },
      {.ping_interval = 1s},
      [&drain](io::memory_stream &client, std::string after) -> task<int> {
        http::websocket_parser from_server{websocket_role::client};

        from_server.feed(after);
        co_await quoll::async::sleep_for(500ms);
        drain.request_stop();

        // Ticks, then the drain's close, answered.
        while ((co_await quoll::test::next_websocket_message(client, from_server)).opcode != websocket_opcode::close) {
        }

        auto const answer = http::format_websocket_close(1001, "", mask);

        (void)co_await io::write(client, std::as_bytes(std::span{answer}));
        static_cast<void>(co_await quoll::test::read_until_closed(client));
        co_return 1;
      },
      {}, drain.get_token());

  REQUIRE(refused == http::error::websocket_closed);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: the connection's stop ends a close waiting behind a copy's send", "[server][websocket]") {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  std::chrono::nanoseconds per_byte{};
  std::chrono::nanoseconds served_at{};
  std::stop_source stop;
  auto const router = socket_at(
      [&per_byte](http::websocket socket) -> task<> {
        auto const sending = [](http::websocket copy) -> task<> {
          static_cast<void>(co_await quoll::test::code_thrown_by(copy.send_text("a long message, 18 s")));
        };

        // A byte every 0.9 s: the send moves, and holds the turn past the handler, outside the connection's stop.
        per_byte = 900ms;
        quoll::async::co_spawn(co_await quoll::async::current_executor(), sending(socket), quoll::async::detached);
        co_await quoll::async::yield();
      },
      {.ping_interval = 0s});

  auto const serving = [&]() -> task<int> {
    co_await http::server{router}.serve_connection(paced_writes{std::move(server_end), per_byte});
    served_at = time.now();
    co_return 1;
  };
  auto const client = [&]() -> task<int> {
    auto const answer = co_await send_then_read(client_end, handshake());

    REQUIRE(answer.response.status == http::status::switching_protocols);
    co_await quoll::async::sleep_for(2s);
    stop.request_stop();
    co_return 1;
  };

  static_cast<void>(quoll::test::run(time, quoll::async::when_all(serving(), client()), stop.get_token()));

  // The handler's close, waiting for the turn, ends with the stop, at 2 s, not with the copy's send.
  REQUIRE(served_at == 2s);
}

TEST_CASE("accept_websocket: a receive() answering a ping behind a long message ends with its stop",
          "[server][websocket]") {
  std::error_code waited;

  auto const [ended_at, reported] = run_paced(
      [&waited](http::websocket socket, std::chrono::nanoseconds &per_byte) -> task<> {
        auto const sending = [&socket]() -> task<> { co_await socket.send_text("a long message, 2 s"); };
        auto const receiving = [&socket, &waited]() -> task<> {
          // The ping read, its pong waits for the turn behind the message, until the timeout at 0.5 s.
          waited = co_await quoll::test::code_thrown_by(quoll::async::with_timeout(500ms, socket.receive()));
        };

        per_byte = 100ms;
        co_await quoll::async::yield();
        static_cast<void>(co_await quoll::async::when_all(sending(), receiving()));
      },
      {.ping_interval = 0s},
      [](io::memory_stream &client, std::string after) -> task<int> {
        auto const rest = expecting({websocket_message{websocket_opcode::text, "a long message, 2 s"},
                                     websocket_message{websocket_opcode::close, "", 1000}});
        auto const ping = http::format_websocket_frame(websocket_opcode::ping, "", true, mask);

        (void)co_await io::write(client, std::as_bytes(std::span{ping}));

        auto const done = co_await rest(client, std::move(after));

        co_return done;
      });

  REQUIRE(waited == std::errc::timed_out);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a handshake as the server drains is answered 503", "[server][websocket]") {
  auto ran = false;
  std::stop_source drain;
  http::connection_options options;
  http::router router;
  quoll::async::run_loop loop;

  // The drain starts as the request is answered: one asked before it would not have been read.
  router.get("/socket", [&ran, &drain](http::request request) {
    drain.request_stop();
    return http::accept_websocket(request, [&ran](http::websocket) -> task<> {
      ran = true;
      co_return;
    });
  });
  options.drain = drain.get_token();

  auto const reported = served_on(
      loop, router,
      [](io::memory_stream &client) -> task<int> {
        auto const answer = co_await send_then_read(client, handshake());

        REQUIRE(answer.response.status == http::status::service_unavailable);
        co_return 1;
      },
      options);

  REQUIRE_FALSE(ran);
  REQUIRE(reported.empty());
}
