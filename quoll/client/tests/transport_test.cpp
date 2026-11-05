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

#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
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
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
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

// Reads a request and answers 200: what it found goes in `host_field`.
task<> answer_once(io::memory_stream stream, std::string &host_field) {
  std::string buffer;
  http::request_parser parser;

  if (co_await http::read(stream, buffer, parser)) {
    co_return;
  }

  host_field = std::string{parser.release().fields.find("Host").value_or("")};

  std::string const response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

  (void)co_await io::write(stream, std::as_bytes(std::span{response}));
}

// What a tls_recording_connector was asked.
struct tls_record {
  bool secure = false;
  std::string destination;
  std::string ca_file;
  std::string host_field;
};

// Keeps what a transport asked of it, and serves its connections in memory.
struct tls_recording_connector {
  // Static: a transport default-constructs the connector of a URL.
  static inline tls_record log;

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    log.secure = false;
    log.destination = host + ':' + std::to_string(port);
    co_return co_await open();
  }

  [[nodiscard]] task<io::any_stream> connect_tls(std::string host, std::uint16_t port,
                                                 io::tls_client_options const &options) const {
    log.secure = true;
    log.destination = host + ':' + std::to_string(port);
    log.ca_file = options.ca_file;
    co_return co_await open();
  }

private:
  [[nodiscard]] static task<io::any_stream> open() {
    auto [client_end, server_end] = io::make_memory_stream_pair();
    async::any_executor const executor{co_await async::current_executor()};

    co_spawn(executor, answer_once(std::move(server_end), log.host_field), async::detached);
    co_return io::any_stream{std::move(client_end)};
  }
};

static_assert(io::TlsConnector<tls_recording_connector>);

// Connects in plain only.
struct plain_connector {
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    throw std::logic_error{"not expected"};
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }
};

static_assert(io::Connector<plain_connector>);
static_assert(!io::TlsConnector<plain_connector>);

using namespace std::chrono_literals;

using quoll::test::virtual_time;

// A network on a virtual clock: connecting takes `connect_delay`, and each connection answers its requests, for as long
// as it lasts, after `response_delay`, as it is when the request arrives.
struct slow_network {
  virtual_time *time;
  std::chrono::nanoseconds connect_delay = 0s;
  std::chrono::nanoseconds response_delay = 0s;
  int connections = 0;
};

task<> answer_slowly(io::memory_stream stream, slow_network &network) {
  std::string buffer;

  for (;;) {
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    co_await async::sleep_for(network.response_delay);

    std::string const response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

    if ((co_await io::write(stream, std::as_bytes(std::span{response}))).error) {
      co_return;
    }
  }
}

class slow_connector {
public:
  explicit slow_connector(slow_network &network) noexcept : network_(&network) {}

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto *const network = network_;

    co_await async::sleep_for(network->connect_delay);

    auto [client_end, server_end] = io::make_memory_stream_pair();

    ++network->connections;
    co_spawn(network->time->get_executor(), answer_slowly(std::move(server_end), *network), async::detached);
    co_return io::any_stream{std::move(client_end)};
  }

private:
  slow_network *network_;
};

static_assert(io::Connector<slow_connector>);

http::request get_root() {
  http::request request;

  request.target = "/";
  return request;
}

// The error `count` requests end with, one after the other on one transport: none if they all succeed. The transport,
// and its idle connections, are gone when the task ends.
task<std::error_code> send_gets(slow_network &network, client::transport_options options, int count = 1) {
  client::transport const transport{slow_connector{network}, "server", 80, options};

  try {
    for (auto sent = 0; sent < count; ++sent) {
      (void)co_await transport.send(get_root());
    }
  } catch (std::system_error const &failure) {
    co_return failure.code();
  }

  co_return std::error_code{};
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

  auto const with_host = roundtrip({std::move(request)});

  REQUIRE(with_host.responses.front().fields.find("X-Host") == "virtual.example.com");
}

TEST_CASE("client::transport: a connection that closes is not reused", "[client]") {
  auto const closed_by_server = roundtrip({make_request(verb::get, "/bye"), make_request(verb::get, "/hello")});

  REQUIRE(closed_by_server.responses[0].body == "bye");
  REQUIRE(closed_by_server.responses[1].body == "hello");
  REQUIRE(closed_by_server.connections == 2);

  auto closing = make_request(verb::get, "/hello");

  closing.fields.add("Connection", "close");

  auto const after_closing = roundtrip({std::move(closing), make_request(verb::get, "/hello")});

  REQUIRE(after_closing.connections == 2);

  auto old = make_request(verb::get, "/hello");

  old.version = http::version::http_1_0;

  auto const after_old = roundtrip({std::move(old), make_request(verb::get, "/hello")});

  REQUIRE(after_old.connections == 2);

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
  REQUIRE_FALSE(plain.secure);

  auto const secure = parse_url("https://example.com");

  REQUIRE(secure.host == "example.com");
  REQUIRE(secure.port == 443);
  REQUIRE(secure.secure);

  auto const secure_with_port = parse_url("HTTPS://[::1]:8443/");

  REQUIRE(secure_with_port.host == "::1");
  REQUIRE(secure_with_port.port == 8443);
  REQUIRE(secure_with_port.secure);

  for (std::string_view const url :
       {"https://", "ftp://example.com", "example.com", "http://", "http://example.com:0", "http://example.com:65536",
        "http://example.com:", "http://example.com:8o", "http://[::1", "http://[::1]8080", "http://[example.com]",
        "http://[192.0.2.1]:80", "http://user@example.com", "http://exa mple.com", "http://example.com?x=1",
        "http://example.com:8080#top"}) {
    REQUIRE_THROWS_AS(client::transport<>{url}, std::invalid_argument);
  }

  auto const with_path = parse_url("https://example.com:8443/api/v1");

  REQUIRE(with_path.host == "example.com");
  REQUIRE(with_path.port == 8443);
  REQUIRE(with_path.prefix == "/api/v1");
  REQUIRE(parse_url("http://example.com/api/").prefix == "/api");
  REQUIRE(parse_url("http://example.com/a%20b").prefix == "/a%20b");
  REQUIRE(parse_url("http://example.com/").prefix.empty());
  REQUIRE(parse_url("http://example.com").prefix.empty());
  REQUIRE(parse_url("http://example.com/@scope/v1:beta").prefix == "/@scope/v1:beta");

  // A path that would not read as itself.
  for (std::string_view const url :
       {"http://example.com/a b",          "http://example.com/a/../b",      "http://example.com/..",
        "http://example.com/./a",          "http://example.com/a//b",        "http://example.com/%zz",
        "http://example.com/%2",           "http://example.com/a\tb",        "http://example.com/a\x7f",
        "http://example.com/%2e%2e/admin", "http://example.com/a%2F..",      "http://example.com/a\\..\\b",
        "http://example.com/a%00",         "http://example.com/a\"b",        "http://example.com/a<b",
        "http://example.com/a{b",          "http://example.com/caf\xc3\xa9", "http://user@example.com/v1",
        "http://example.com/v1?x=1",       "http://example.com/v1#top"}) {
    CAPTURE(url);
    REQUIRE_THROWS_AS(client::transport<>{url}, std::invalid_argument);
  }

  REQUIRE_NOTHROW(client::transport<>{"http://127.0.0.1:8080"});
  REQUIRE_NOTHROW(client::transport<>{"https://127.0.0.1:8443"});
}

TEST_CASE("client::transport: an https URL goes through connect_tls, with the options", "[client][tls]") {
  auto const send = [](std::string url, client::transport_options options) {
    tls_recording_connector::log = {};

    auto const call = [&]() -> task<int> {
      client::transport<tls_recording_connector> const transport{url, options};

      co_return static_cast<int>((co_await transport.send(make_request(verb::get, "/"))).status);
    };

    return quoll::test::run(call());
  };

  client::transport_options options;

  options.tls.ca_file = "authority.pem";

  REQUIRE(send("https://example.com", options) == 200);
  REQUIRE(tls_recording_connector::log.secure);
  REQUIRE(tls_recording_connector::log.destination == "example.com:443");
  REQUIRE(tls_recording_connector::log.ca_file == "authority.pem");
  REQUIRE(tls_recording_connector::log.host_field == "example.com");

  REQUIRE(send("https://example.com:80", {}) == 200);
  REQUIRE(tls_recording_connector::log.host_field == "example.com:80");

  REQUIRE(send("http://example.com", {}) == 200);
  REQUIRE_FALSE(tls_recording_connector::log.secure);
  REQUIRE(tls_recording_connector::log.destination == "example.com:80");
  REQUIRE(tls_recording_connector::log.host_field == "example.com");

  REQUIRE(send("http://example.com:443", {}) == 200);
  REQUIRE(tls_recording_connector::log.host_field == "example.com:443");
}

TEST_CASE("client::transport: an https URL needs a connector that opens TLS connections", "[client][tls]") {
  REQUIRE_THROWS_AS(client::transport<plain_connector>{"https://example.com"}, std::invalid_argument);
  REQUIRE_NOTHROW(client::transport<plain_connector>{"http://example.com"});
}

TEST_CASE("client::transport: the default connector needs an executor offering a network", "[client]") {
  client::transport<> const transport{"http://127.0.0.1:8080"};

  REQUIRE_THROWS_AS(quoll::test::run(transport.send(make_request(verb::get, "/"))), std::logic_error);
}

TEST_CASE("client::transport: a connection that takes too long fails with timed_out", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 2s})) == std::errc::timed_out);
  REQUIRE(time.now() == 2s);
  REQUIRE(network.connections == 0);
}

TEST_CASE("client::transport: a response that takes too long fails with timed_out", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 1h};
  std::chrono::nanoseconds failed_at{};

  auto const send = [&]() -> task<std::error_code> {
    auto const error = co_await send_gets(network, {.response_timeout = 3s});

    failed_at = time.now();
    co_return error;
  };

  REQUIRE(quoll::test::run(time, send()) == std::errc::timed_out);
  REQUIRE(failed_at == 3s);
  REQUIRE(network.connections == 1);
}

TEST_CASE("client::transport: requests that end in time succeed, on one connection", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1s, .response_delay = 1s};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 2s, .response_timeout = 2s}, 3)) ==
          std::error_code{});
  REQUIRE(time.now() == 4s);
  REQUIRE(network.connections == 1);
}

TEST_CASE("client::transport: a request timing out on a kept connection is not sent again, nor the connection reused",
          "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 1s};

  std::error_code error;

  auto const requests = [&]() -> task<int> {
    client::transport const transport{slow_connector{network}, "server", 80, {.response_timeout = 2s}};

    (void)co_await transport.send(get_root());
    network.response_delay = 1h;

    try {
      (void)co_await transport.send(get_root());
    } catch (std::system_error const &failure) {
      error = failure.code();
    }

    auto const after_timeout = network.connections;

    network.response_delay = 1s;
    (void)co_await transport.send(get_root());
    co_return after_timeout;
  };

  REQUIRE(quoll::test::run(time, requests()) == 1);
  REQUIRE(error == std::errc::timed_out);
  REQUIRE(network.connections == 2);
}

TEST_CASE("client::transport: the default timeouts apply", "[client][timer]") {
  virtual_time connecting;
  virtual_time responding;
  slow_network never_connects{.time = &connecting, .connect_delay = 1h};
  slow_network never_answers{.time = &responding, .response_delay = 1h};

  REQUIRE(quoll::test::run(connecting, send_gets(never_connects, {})) == std::errc::timed_out);
  REQUIRE(connecting.now() == 30s);

  std::chrono::nanoseconds failed_at{};

  auto const send = [&]() -> task<std::error_code> {
    auto const error = co_await send_gets(never_answers, {});

    failed_at = responding.now();
    co_return error;
  };

  REQUIRE(quoll::test::run(responding, send()) == std::errc::timed_out);
  REQUIRE(failed_at == 60s);
}

TEST_CASE("client::transport: a negative timeout has expired, rather than lifting the limit", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = -1s})) == std::errc::timed_out);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("client::transport: a timeout of zero does not apply", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h, .response_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 0s, .response_timeout = 0s})) ==
          std::error_code{});
  REQUIRE(time.now() == 2h);
}

TEST_CASE("client::transport: a stop request is no timeout", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 1h};
  std::stop_source source;

  auto const stop_soon = [&]() -> task<> {
    co_await async::sleep_for(1s);
    source.request_stop();
  };

  auto const outcome = quoll::test::run(time, async::when_all(send_gets(network, {}), stop_soon()), source.get_token());

  REQUIRE(std::get<0>(outcome.value()) == std::errc::operation_canceled);
}
