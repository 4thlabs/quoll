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
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

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

// The next message `client` carries from a server.
task<websocket_message> next_from_server(io::memory_stream &client, http::websocket_parser &parser) {
  std::array<char, 256> chunk;

  for (;;) {
    if (auto message = parser.next()) {
      co_return std::move(*message);
    }

    auto const read = co_await client.read_some(std::as_writable_bytes(std::span{chunk}));

    if (read.error) {
      throw std::system_error{read.error};
    }

    parser.feed(std::string_view{chunk.data(), read.transferred});
  }
}

// Each message until the close, recording how receiving ended.
task<> receive_all(http::websocket socket, std::error_code &ended) {
  try {
    for (;;) {
      if ((co_await socket.receive()).opcode == websocket_opcode::close) {
        co_return;
      }
    }
  } catch (std::system_error const &thrown) {
    ended = thrown.code();
    throw;
  }
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

TEST_CASE("serve_connection: what a WebSocket handler throws is reported, but for a stop", "[server][websocket]") {
  SECTION("a failure") {
    auto const router = socket_at([](http::websocket) -> task<> { throw std::runtime_error{"handler failure"}; });

    auto const reported = served(router, [](io::memory_stream &client) -> task<int> {
      auto answer = co_await send_then_read(client, handshake());
      http::websocket_parser from_server{websocket_role::client};

      from_server.feed(answer.after);

      auto const close = co_await next_from_server(client, from_server);

      REQUIRE(answer.response.status == http::status::switching_protocols);
      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1011});
      co_return 1;
    });

    REQUIRE(reported == std::vector<std::string>{"handler failure"});
  }

  SECTION("a send after the peer's close") {
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
      auto const sent = handshake(http::format_websocket_close(1000, "", mask));
      auto const answer = co_await send_then_read(client, sent);

      REQUIRE(answer.response.status == http::status::switching_protocols);
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
    REQUIRE(reported.empty());
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

      auto const close = co_await next_from_server(client, from_server);

      REQUIRE(close == websocket_message{websocket_opcode::close, "", 1002});
      co_return 1;
    });

    REQUIRE(ended == http::error::websocket_protocol);
    REQUIRE(reported.empty());
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
    auto const first = co_await next_from_server(client, from_server);
    auto const first_at = time.now();
    auto const pong = http::format_websocket_frame(websocket_opcode::pong, "", true, mask);

    (void)co_await io::write(client, std::as_bytes(std::span{pong}));

    // The pong counted: not pinged at the next tick, but at the one after.
    auto const second = co_await next_from_server(client, from_server);
    auto const second_at = time.now();
    // No answer: gone at the next tick, and the connection with it.
    auto const rest = co_await quoll::test::read_until_closed(client);

    REQUIRE(first.opcode == websocket_opcode::ping);
    REQUIRE(first_at == 1s);
    REQUIRE(second.opcode == websocket_opcode::ping);
    REQUIRE(second_at == 3s);
    REQUIRE(rest.empty());
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

        auto const close = co_await next_from_server(client, from_server);
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

// A server's stream whose writes, once `stalled`, take nothing until they are stopped: a peer that no longer reads.
class stalling {
public:
  stalling(io::memory_stream inner, bool const &stalled) noexcept : inner_(std::move(inner)), stalled_(&stalled) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (*stalled_) {
      try {
        co_await quoll::async::sleep_for(24h);
      } catch (std::system_error const &stopped) {
        co_return io::io_result{stopped.code(), 0};
      }
    }

    co_return co_await inner_.write_some(data);
  }

  [[nodiscard]] task<io::io_result> shutdown_send() { co_return co_await inner_.shutdown_send(); }

private:
  io::memory_stream inner_;
  bool const *stalled_;
};

// When the connection ended on `time`, its handler, which stalls the stream, run by a server over a `stalling` stream
// with a ping every second, and what the server reported.
template <class Handler>
std::pair<std::chrono::nanoseconds, std::vector<std::string>> stalled_end(Handler handler) {
  quoll::test::virtual_time time;
  auto [client_end, server_end] = io::make_memory_stream_pair();
  auto stalled = false;
  std::vector<std::string> reported;
  http::connection_options options;
  auto const router =
      socket_at([&stalled, &handler](http::websocket socket) { return handler(std::move(socket), stalled); },
                {.ping_interval = 1s});

  options.on_handler_error = [&reported](std::exception_ptr const &failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &thrown) {
      reported.emplace_back(thrown.what());
    }
  };

  auto const client = [&]() -> task<int> {
    auto const answer = co_await send_then_read(client_end, handshake());

    REQUIRE(answer.response.status == http::status::switching_protocols);
    static_cast<void>(co_await quoll::test::read_until_closed(client_end));
    co_return 1;
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(http::server{router, {.connection = options}}.serve_connection(
                                                        stalling{std::move(server_end), stalled}),
                                                    client()));

  REQUIRE(std::get<1>(outcome.value()) == 1);
  return {time.now(), reported};
}

} // namespace

TEST_CASE("accept_websocket: a peer that takes nothing has its handler stopped", "[server][websocket]") {
  std::error_code ended;

  auto const [ended_at, reported] = stalled_end([&ended](http::websocket socket, bool &stalled) -> task<> {
    stalled = true;

    try {
      co_await socket.send_text("never taken");
    } catch (std::system_error const &thrown) {
      ended = thrown.code();
      throw;
    }
  });

  // Pinged at 1 s, the ping waits behind the message: not taken at 2 s.
  REQUIRE(ended_at == 2s);
  REQUIRE(ended == std::errc::operation_canceled);
  REQUIRE(reported.empty());
}

TEST_CASE("accept_websocket: a ping still pending as the handler ends is no failure", "[server][websocket]") {
  auto const [ended_at, reported] = stalled_end([](http::websocket, bool &stalled) -> task<> {
    stalled = true;
    co_await quoll::async::sleep_for(1500ms);
  });

  // Over at 1.5 s, then a second for the peer's close, which does not come.
  REQUIRE(ended_at == 2500ms);
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
