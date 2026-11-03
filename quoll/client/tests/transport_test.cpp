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
#include <quoll/client.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace client = quoll::client;
namespace http = quoll::http;
namespace io = quoll::io;

using async::task;
using http::verb;

// The server end of the connections opened by a memory_connector: each one is served on the loop by `serve`.
struct memory_server {
  async::run_loop::executor executor;
  std::function<task<>(io::memory_stream)> serve;
  int connections = 0;
  // Where the last connection went, as `host:port`.
  std::string destination;
  std::exception_ptr failure;
};

class memory_connector {
public:
  explicit memory_connector(memory_server &server) noexcept : server_(&server) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    auto *const server = server_;
    auto [client_end, server_end] = io::make_memory_stream_pair();

    ++server->connections;
    server->destination = host + ':' + std::to_string(port);
    co_spawn(
        server->executor, server->serve(std::move(server_end)),
        quoll::test::overloaded{[] {}, [server](std::exception_ptr error) { server->failure = std::move(error); }});
    co_return io::any_stream{std::move(client_end)};
  }

private:
  memory_server *server_;
};

static_assert(io::Connector<memory_connector>);
static_assert(http::HttpTransport<client::transport<memory_connector>>);

http::router make_router() {
  http::router router;

  router.add(verb::get, "/hello", "hello", [](http::request request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = "hello";
    response.fields.add("X-Host", request.fields.find("Host").value_or(""));
    co_return response;
  });
  auto const echo = [](http::request request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = std::move(request.body);
    co_return response;
  };

  router.add(verb::post, "/echo", "echo.post", echo);
  router.add(verb::put, "/echo", "echo.put", echo);
  router.add(verb::get, "/bye", "bye", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.fields.add("Connection", "close");
    response.body = "bye";
    co_return response;
  });
  return router;
}

// A server reading each request and answering it with the next of `replies`, raw. Once out of replies, it waits for
// one more request (`then_read`) or not, and closes.
task<> answer(io::memory_stream stream, std::vector<std::string> replies, bool then_read) {
  std::string buffer;

  for (auto const &reply : replies) {
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    (void)parser.release();
    (void)co_await io::write(stream, std::as_bytes(std::span{reply}));
  }

  if (then_read) {
    http::request_parser parser;

    (void)co_await http::read(stream, buffer, parser);
  }
}

std::function<task<>(io::memory_stream)> answering(std::vector<std::string> replies, bool then_read = false) {
  return [replies = std::move(replies), then_read](io::memory_stream stream) {
    return answer(std::move(stream), replies, then_read);
  };
}

http::request make_request(verb method, std::string target, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  request.body = std::move(body);
  return request;
}

struct origin {
  std::string host = "example.com";
  std::uint16_t port = 80;
};

struct outcome {
  std::vector<http::response> responses;
  // What the request that failed threw.
  std::error_code error;
  int connections = 0;
  std::string destination;
};

// Sends `requests` in turn on one transport; `error` is what the first one to fail threw. The transport is gone when
// the task ends, closing its idle connections while the loop still runs.
task<std::vector<http::response>> send_all(memory_server &server, std::vector<http::request> requests, origin to,
                                           client::transport_options options, std::error_code &error) {
  client::transport transport{memory_connector{server}, to.host, to.port, options};
  std::vector<http::response> responses;

  for (auto &request : requests) {
    try {
      responses.push_back(co_await transport.send(std::move(request)));
    } catch (std::system_error const &failure) {
      if (!error) {
        error = failure.code();
      }
    }
  }

  co_return responses;
}

outcome roundtrip(std::function<task<>(io::memory_stream)> serve, std::vector<http::request> requests, origin to = {},
                  client::transport_options options = {}) {
  async::run_loop loop;
  memory_server server{loop.get_executor(), std::move(serve), 0, {}, {}};
  outcome result;
  auto responses = quoll::test::run(loop, send_all(server, std::move(requests), std::move(to), options, result.error));

  if (server.failure) {
    std::rethrow_exception(server.failure);
  }

  result.responses = std::move(responses.value());
  result.connections = server.connections;
  result.destination = server.destination;
  return result;
}

outcome roundtrip(std::vector<http::request> requests, origin to = {}, client::transport_options options = {}) {
  auto router = std::make_shared<http::router const>(make_router());

  return roundtrip(
      [router](io::memory_stream stream) { return quoll::server::serve_connection(std::move(stream), *router); },
      std::move(requests), std::move(to), options);
}

} // namespace

TEST_CASE("client::transport: requests reuse a connection kept alive", "[client]") {
  auto const result = roundtrip(
      {make_request(verb::get, "/hello"), make_request(verb::put, "/echo", "ping"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses.size() == 3);
  REQUIRE(result.responses[0].body == "hello");
  REQUIRE(result.responses[1].body == "ping");
  REQUIRE(result.responses[2].body == "hello");
  REQUIRE(result.connections == 1);
  REQUIRE(result.destination == "example.com:80");
}

TEST_CASE("client::transport: a POST opens its own connection, kept for the next requests", "[client]") {
  auto const result = roundtrip({make_request(verb::get, "/hello"), make_request(verb::post, "/echo", "ping"),
                                 make_request(verb::get, "/hello"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses.size() == 4);
  REQUIRE(result.responses[1].body == "ping");
  REQUIRE(result.connections == 2);
}

TEST_CASE("client::transport: the Host field names the server, and its port unless it is 80", "[client]") {
  REQUIRE(roundtrip({make_request(verb::get, "/hello")}).responses.front().fields.find("X-Host") == "example.com");

  auto const ipv6 = roundtrip({make_request(verb::get, "/hello")}, {"::1", 8080});

  REQUIRE(ipv6.responses.front().fields.find("X-Host") == "[::1]:8080");
  REQUIRE(ipv6.destination == "::1:8080");

  auto request = make_request(verb::get, "/hello");

  request.fields.add("Host", "virtual.example.com");
  REQUIRE(roundtrip({std::move(request)}).responses.front().fields.find("X-Host") == "virtual.example.com");
}

TEST_CASE("client::transport: a connection that closes is not reused", "[client]") {
  auto const closed_by_server = roundtrip({make_request(verb::get, "/bye"), make_request(verb::get, "/hello")});

  REQUIRE(closed_by_server.responses[0].body == "bye");
  REQUIRE(closed_by_server.responses[1].body == "hello");
  REQUIRE(closed_by_server.connections == 2);

  auto closing = make_request(verb::get, "/hello");

  closing.fields.add("Connection", "close");
  REQUIRE(roundtrip({std::move(closing), make_request(verb::get, "/hello")}).connections == 2);

  auto old = make_request(verb::get, "/hello");

  old.version = http::version::http_1_0;
  REQUIRE(roundtrip({std::move(old), make_request(verb::get, "/hello")}).connections == 2);

  auto const no_idle = roundtrip({make_request(verb::get, "/hello"), make_request(verb::get, "/hello")}, {},
                                 {.limits = {}, .max_idle_connections = 0});

  REQUIRE(no_idle.responses.size() == 2);
  REQUIRE(no_idle.connections == 2);
}

TEST_CASE("client::transport: a response to HEAD has no body, and the connection goes on", "[client]") {
  auto const result = roundtrip({make_request(verb::head, "/hello"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE(result.responses[0].body.empty());
  REQUIRE(result.responses[1].body == "hello");
  REQUIRE(result.connections == 1);
}

TEST_CASE("client::transport: interim responses are skipped", "[client]") {
  auto const result = roundtrip(answering({"HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"
                                           "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}),
                                {make_request(verb::get, "/")});

  REQUIRE(result.responses.size() == 1);
  REQUIRE(result.responses.front().status == http::status::ok);
  REQUIRE(result.responses.front().body == "ok");

  std::string endless;

  for (int index = 0; index < 9; ++index) {
    endless += "HTTP/1.1 100 Continue\r\n\r\n";
  }

  REQUIRE(roundtrip(answering({endless}), {make_request(verb::get, "/")}).error == http::error::bad_message);
}

TEST_CASE("client::transport: an idempotent request goes again when its kept connection turns out closed", "[client]") {
  std::string const ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

  // Closed before the request is written, or once it is read.
  for (auto const then_read : {false, true}) {
    auto const result =
        roundtrip(answering({ok}, then_read), {make_request(verb::get, "/"), make_request(verb::delete_, "/")});

    REQUIRE_FALSE(result.error);
    REQUIRE(result.responses.size() == 2);
    REQUIRE(result.responses[1].body == "ok");
    REQUIRE(result.connections == 2);
  }

  // A POST, which cannot be sent twice, does not take the idle connection.
  auto const post = roundtrip(answering({ok}, true), {make_request(verb::get, "/"), make_request(verb::post, "/")});

  REQUIRE_FALSE(post.error);
  REQUIRE(post.responses.size() == 2);
  REQUIRE(post.connections == 2);
}

namespace {

// Answers a request, then times the connection out: a 408 the client reads as the response to its next request.
task<> answer_then_time_out(io::memory_stream stream) {
  std::string buffer;
  http::request_parser parser;

  if (co_await http::read(stream, buffer, parser)) {
    co_return;
  }

  (void)parser.release();

  std::string const ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  std::string const timeout = "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

  (void)co_await io::write(stream, std::as_bytes(std::span{ok}));
  // The client reads its response, and keeps the connection, before the 408 comes.
  co_await async::yield();
  (void)co_await io::write(stream, std::as_bytes(std::span{timeout}));
  (void)co_await http::read(stream, buffer, parser);
}

} // namespace

TEST_CASE("client::transport: a 408 on an idle connection sends the request again", "[client]") {
  auto const result = roundtrip(answer_then_time_out, {make_request(verb::get, "/"), make_request(verb::get, "/")});

  REQUIRE_FALSE(result.error);
  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.responses[1].status == http::status::ok);
  REQUIRE(result.connections == 2);
}

TEST_CASE("client::transport: an idle connection found closed closes the older idle ones", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  auto served = 0;
  // The first two connections answer once, then close; the next ones serve the router.
  memory_server server{loop.get_executor(),
                       [&](io::memory_stream stream) {
                         return ++served <= 2 ? answer(std::move(stream),
                                                       {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, false)
                                              : quoll::server::serve_connection(std::move(stream), router);
                       },
                       0,
                       {},
                       {}};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    client::transport transport{memory_connector{target}, "example.com", 80};
    auto [first, second] = co_await async::when_all(transport.send(make_request(verb::get, "/")),
                                                    transport.send(make_request(verb::get, "/")));
    // Finds the last idle connection closed, closes the other one, and connects.
    auto third = co_await transport.send(make_request(verb::get, "/hello"));
    // Reuses the new connection.
    auto fourth = co_await transport.send(make_request(verb::get, "/hello"));

    co_return std::vector<std::string>{first.body, second.body, third.body, fourth.body};
  }(server));

  REQUIRE(bodies == std::vector<std::string>{"ok", "ok", "hello", "hello"});
  REQUIRE(server.connections == 3);
}

TEST_CASE("client::transport: a response that cannot be read fails, and its connection is dropped", "[client]") {
  // The second request of each case connects again.
  auto const truncated = roundtrip(answering({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"}),
                                   {make_request(verb::get, "/"), make_request(verb::get, "/")});

  REQUIRE(truncated.responses.empty());
  REQUIRE(truncated.error == http::error::partial_message);
  REQUIRE(truncated.connections == 2);

  auto const too_large = roundtrip({make_request(verb::get, "/hello"), make_request(verb::get, "/hello")}, {},
                                   {.limits = {.max_body_size = 2}, .max_idle_connections = 8});

  REQUIRE(too_large.responses.empty());
  REQUIRE(too_large.error == http::error::body_too_large);
  REQUIRE(too_large.connections == 2);
}

TEST_CASE("client::transport: a connection with bytes past its response is not reused", "[client]") {
  std::string const extra = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1";
  auto const result =
      roundtrip(answering({extra, extra}), {make_request(verb::get, "/"), make_request(verb::get, "/")});

  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.connections == 2);
}

TEST_CASE("client::transport: concurrent requests open their own connections, kept for the next ones", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  memory_server server{
      loop.get_executor(),
      [&router](io::memory_stream stream) { return quoll::server::serve_connection(std::move(stream), router); },
      0,
      {},
      {}};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    client::transport transport{memory_connector{target}, "example.com", 80};
    auto [first, second] = co_await async::when_all(transport.send(make_request(verb::get, "/hello")),
                                                    transport.send(make_request(verb::put, "/echo", "ping")));
    auto third = co_await transport.send(make_request(verb::get, "/hello"));

    co_return std::vector<std::string>{first.body, second.body, third.body};
  }(server));

  REQUIRE(bodies == std::vector<std::string>{"hello", "ping", "hello"});
  REQUIRE(server.connections == 2);
}

TEST_CASE("client::transport: what cannot be sent is refused before the task runs", "[client]") {
  async::run_loop loop;
  memory_server server{loop.get_executor(), answering({}), 0, {}, {}};

  for (std::string_view const host : {"",
                                      "a b",
                                      "user@example.com",
                                      "[::1]",
                                      "example.com/path",
                                      "localhost:8080",
                                      "fe80::1%eth0",
                                      "a\x7f",
                                      "tab\there",
                                      "a,b",
                                      "a<b",
                                      ":",
                                      "a:b",
                                      "cafe:babe",
                                      ":::",
                                      "1::2::3",
                                      "1:2:3:4:5:6:7:8:9",
                                      "1:2:3:4:5:6:7",
                                      "1:",
                                      "::1.2.3",
                                      "::1.2.3.256",
                                      "::12345"}) {
    REQUIRE_THROWS_AS((client::transport{memory_connector{server}, std::string{host}, 80}), std::invalid_argument);
  }

  REQUIRE_THROWS_AS((client::transport{memory_connector{server}, std::string{"nul\0", 4}, 80}), std::invalid_argument);
  for (std::string_view const host : {"my-host_1.example.com", "192.0.2.1", "2001:DB8::1", "::ffff:192.0.2.1",
                                      "::", "1::", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:192.0.2.1"}) {
    REQUIRE_NOTHROW(client::transport{memory_connector{server}, std::string{host}, 80});
  }

  client::transport const transport{memory_connector{server}, "example.com", 80};
  auto field = make_request(verb::get, "/");

  field.fields.add("X-Split", "a\r\nInjected: b");
  REQUIRE_THROWS_AS(transport.send(make_request(verb::get, "http://example.com/")), std::invalid_argument);
  REQUIRE_THROWS_AS(transport.send(std::move(field)), std::invalid_argument);
  REQUIRE(server.connections == 0);
}

TEST_CASE("client::transport: built from an http URL", "[client]") {
  using quoll::client::detail::parse_url;

  auto const plain = parse_url("http://example.com");

  REQUIRE(plain.host == "example.com");
  REQUIRE(plain.port == 80);

  auto const with_port = parse_url("HTTP://Example.com:8080/");

  REQUIRE(with_port.host == "Example.com");
  REQUIRE(with_port.port == 8080);

  auto const ipv6 = parse_url("http://[::1]:8080");

  REQUIRE(ipv6.host == "::1");
  REQUIRE(ipv6.port == 8080);
  REQUIRE(parse_url("http://[2001:db8::1]").port == 80);

  for (std::string_view const url :
       {"https://example.com", "ftp://example.com", "example.com", "http://", "http://example.com/api",
        "http://example.com:0", "http://example.com:65536", "http://example.com:", "http://example.com:8o",
        "http://[::1", "http://[::1]8080", "http://[example.com]", "http://[192.0.2.1]:80", "http://user@example.com",
        "http://exa mple.com", "http://example.com?x=1", "http://example.com:8080#top"}) {
    REQUIRE_THROWS_AS(client::transport<>{url}, std::invalid_argument);
  }

  REQUIRE_NOTHROW(client::transport<>{"http://127.0.0.1:8080"});
}

TEST_CASE("client::transport: the default connector needs an executor offering a network", "[client]") {
  client::transport<> const transport{"http://127.0.0.1:8080"};

  REQUIRE_THROWS_AS(quoll::test::run(transport.send(make_request(verb::get, "/"))), std::logic_error);
}
