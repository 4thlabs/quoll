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
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/client.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/event_writer.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/memory_network.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstring>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
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

namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;

using async::task;
using http::verb;

using quoll::test::memory_connector;
using quoll::test::memory_server;
using quoll::test::serving;
static_assert(http::HttpTransport<http::client>);

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

// Sends `requests` in turn on one client; `error` is what the first one to fail threw. The client is gone when
// the task ends, closing its idle connections while the loop still runs.
task<std::vector<http::response>> send_all(memory_server &server, std::vector<http::request> requests, origin to,
                                           http::client_options options, std::error_code &error) {
  http::client client{memory_connector{server}, to.host, to.port, options};
  std::vector<http::response> responses;

  for (auto &request : requests) {
    try {
      responses.push_back(co_await client.send(std::move(request)));
    } catch (std::system_error const &failure) {
      if (!error) {
        error = failure.code();
      }
    }
  }

  co_return responses;
}

outcome roundtrip(std::function<task<>(io::memory_stream)> serve, std::vector<http::request> requests, origin to = {},
                  http::client_options options = {}) {
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

outcome roundtrip(std::vector<http::request> requests, origin to = {}, http::client_options options = {}) {
  auto router = std::make_shared<http::router const>(make_router());

  return roundtrip(
      [router](io::memory_stream stream) { return quoll::http::server{*router}.serve_connection(std::move(stream)); },
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

// Keeps what a client asked of it, and serves its connections in memory.
struct tls_recording_connector {
  // Static: a client default-constructs the connector of a URL.
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
  // What to answer first, raw, before the plain answers.
  std::vector<std::string> first_replies{};
};

task<> answer_slowly(io::memory_stream stream, slow_network &network) {
  std::string buffer;

  for (;;) {
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    co_await async::sleep_for(network.response_delay);

    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

    if (!network.first_replies.empty()) {
      response = std::move(network.first_replies.front());
      network.first_replies.erase(network.first_replies.begin());
    }

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

// The error `count` requests end with, one after the other on one client: none if they all succeed. The client,
// and its idle connections, are gone when the task ends.
task<std::error_code> send_gets(slow_network &network, http::client_options options, int count = 1) {
  http::client const client{slow_connector{network}, "server", 80, options};

  try {
    for (auto sent = 0; sent < count; ++sent) {
      (void)co_await client.send(get_root());
    }
  } catch (std::system_error const &failure) {
    co_return failure.code();
  }

  co_return std::error_code{};
}

// Pieces of `size` `count` times, counting its runs.
http::body_writer pieces_of(std::size_t size, int count, int *runs = nullptr) {
  return [size, count, runs](http::body_stream out) -> task<> {
    std::string const piece(size, 'x');

    if (runs != nullptr) {
      ++*runs;
    }

    for (auto index = 0; index < count; ++index) {
      co_await out.write(piece);
    }
  };
}

http::request upload_of(http::body_writer writer, verb method = verb::post, std::string target = "/echo") {
  auto request = make_request(method, std::move(target));

  request.body_writer = std::move(writer);
  return request;
}

} // namespace

TEST_CASE("http::client: requests reuse a connection kept alive", "[client]") {
  auto const result = roundtrip(
      {make_request(verb::get, "/hello"), make_request(verb::put, "/echo", "ping"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses.size() == 3);
  REQUIRE(result.responses[0].body == "hello");
  REQUIRE(result.responses[1].body == "ping");
  REQUIRE(result.responses[2].body == "hello");
  REQUIRE(result.connections == 1);
  REQUIRE(result.destination == "example.com:80");
}

TEST_CASE("http::client: a POST takes an idle connection, as any request whose body is whole", "[client]") {
  auto const result = roundtrip({make_request(verb::get, "/hello"), make_request(verb::post, "/echo", "ping"),
                                 make_request(verb::get, "/hello"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses.size() == 4);
  REQUIRE(result.responses[1].body == "ping");
  REQUIRE(result.connections == 1);
}

TEST_CASE("http::client: the Host field names the server, and its port unless it is 80", "[client]") {
  REQUIRE(roundtrip({make_request(verb::get, "/hello")}).responses.front().fields.find("X-Host") == "example.com");

  auto const ipv6 = roundtrip({make_request(verb::get, "/hello")}, {"::1", 8080});

  REQUIRE(ipv6.responses.front().fields.find("X-Host") == "[::1]:8080");
  REQUIRE(ipv6.destination == "::1:8080");

  auto request = make_request(verb::get, "/hello");

  request.fields.add("Host", "virtual.example.com");

  auto const with_host = roundtrip({std::move(request)});

  REQUIRE(with_host.responses.front().fields.find("X-Host") == "virtual.example.com");
}

TEST_CASE("http::client: a connection that closes is not reused", "[client]") {
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

TEST_CASE("http::client: a response to HEAD has no body, and the connection goes on", "[client]") {
  auto const result = roundtrip({make_request(verb::head, "/hello"), make_request(verb::get, "/hello")});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE(result.responses[0].body.empty());
  REQUIRE(result.responses[1].body == "hello");
  REQUIRE(result.connections == 1);
}

TEST_CASE("http::client: interim responses are skipped", "[client]") {
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

TEST_CASE("http::client: an idempotent request goes again when its kept connection turns out closed", "[client]") {
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

  // A POST takes the idle connection too, but is not sent again once the server may have read it: the stream's
  // failure is the request's, for the caller to decide on.
  auto const post = roundtrip(answering({ok}, true), {make_request(verb::get, "/"), make_request(verb::post, "/")});

  REQUIRE(post.error == io::error::end_of_stream);
  REQUIRE(post.responses.size() == 1);
  REQUIRE(post.connections == 1);

  // One the server closed before it was written could not be read whole: it goes again.
  auto const unsent = roundtrip(answering({ok}, false), {make_request(verb::get, "/"), make_request(verb::post, "/")});

  REQUIRE_FALSE(unsent.error);
  REQUIRE(unsent.responses.size() == 2);
  REQUIRE(unsent.connections == 2);
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

TEST_CASE("http::client: a 408 on an idle connection sends the request again", "[client]") {
  auto const result = roundtrip(answer_then_time_out, {make_request(verb::get, "/"), make_request(verb::get, "/")});

  REQUIRE_FALSE(result.error);
  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.responses[1].status == http::status::ok);
  REQUIRE(result.connections == 2);
}

namespace {

// Answers a request, then times the connection out with a 408 that does not close it, then answers what it reads next:
// a server that would answer a request after the 408 it sent as the connection was idle.
task<> answer_then_time_out_open(io::memory_stream stream) {
  std::string buffer;
  http::request_parser parser;

  if (co_await http::read(stream, buffer, parser)) {
    co_return;
  }

  (void)parser.release();

  std::string const ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  std::string const timeout = "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n\r\n";
  std::string const late = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlate";

  (void)co_await io::write(stream, std::as_bytes(std::span{ok}));
  co_await async::yield();
  (void)co_await io::write(stream, std::as_bytes(std::span{timeout}));

  if (co_await http::read(stream, buffer, parser)) {
    co_return;
  }

  (void)parser.release();

  // After the client has read the 408 alone, and gone on.
  for (auto turn = 0; turn < 16; ++turn) {
    co_await async::yield();
  }

  (void)co_await io::write(stream, std::as_bytes(std::span{late}));
  (void)co_await http::read(stream, buffer, parser);
}

} // namespace

TEST_CASE("http::client: a 408 sent on an idle connection is found before a POST takes it", "[client]") {
  // Closing the connection or not: the check sees bytes nothing asked for, and the POST goes on a new connection.
  auto const serve =
      GENERATE(as<std::function<task<>(io::memory_stream)>>{}, answer_then_time_out, answer_then_time_out_open);
  auto const streamed = GENERATE(false, true);
  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto const posting = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto const first = co_await client.send(make_request(verb::get, "/"));

    REQUIRE(first.body == "ok");

    // The 408 has come.
    for (auto turn = 0; turn < 16; ++turn) {
      co_await async::yield();
    }

    if (!streamed) {
      co_return (co_await client.send(make_request(verb::post, "/"))).body;
    }

    auto posted = co_await client.stream(make_request(verb::post, "/"));
    std::string body;

    while (auto const piece = co_await posted.body.read()) {
      body += *piece;
    }

    co_return body;
  };

  REQUIRE(quoll::test::run(loop, posting()) == "ok");
  REQUIRE(server.connections == 2);
}

namespace {

// Answers a request with `reply`, then sends `last` once the client has kept the connection, and closes, reading
// nothing more.
std::function<task<>(io::memory_stream)> answering_then_closing(std::string reply, std::string last) {
  return [reply = std::move(reply), last = std::move(last)](io::memory_stream stream) -> task<> {
    return [](io::memory_stream on, std::string first, std::string then) -> task<> {
      std::string buffer;
      http::request_parser parser;

      if (co_await http::read(on, buffer, parser)) {
        co_return;
      }

      (void)co_await io::write(on, std::as_bytes(std::span{first}));

      // The client reads its response, and keeps the connection, before the last message comes.
      for (auto turn = 0; turn < 4; ++turn) {
        co_await async::yield();
      }

      (void)co_await io::write(on, std::as_bytes(std::span{then}));
    }(std::move(stream), reply, last);
  };
}

auto const service_unavailable = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
auto const closing_timeout = "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

} // namespace

TEST_CASE("http::client: an idle connection the server closed is found before a POST takes it, whatever it sent",
          "[client]") {
  auto const ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

  // What the server sent before closing answers no request of it.
  for (auto const *const last : {service_unavailable, closing_timeout}) {
    async::run_loop loop;
    memory_server server{loop.get_executor(), answering_then_closing(ok, last), 0, {}, {}};

    auto const posting = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};
      auto const first = co_await client.send(make_request(verb::get, "/"));

      REQUIRE(first.body == "ok");

      // The server has had the time to send its last message and go.
      for (auto turn = 0; turn < 16; ++turn) {
        co_await async::yield();
      }

      co_return (co_await client.send(make_request(verb::post, "/"))).body;
    };

    REQUIRE(quoll::test::run(loop, posting()) == "ok");
    REQUIRE(server.connections == 2);
  }
}

namespace {

// Answers a request, then counts in `late` a request it reads after, before closing.
task<> answer_then_count(io::memory_stream stream, int &late) {
  std::string buffer;
  http::request_parser parser;

  if (co_await http::read(stream, buffer, parser)) {
    co_return;
  }

  (void)parser.release();

  std::string const ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

  (void)co_await io::write(stream, std::as_bytes(std::span{ok}));

  http::request_parser next;

  if (!co_await http::read(stream, buffer, next)) {
    ++late;
  }
}

task<> stream_to_end(http::client client, http::request request) {
  auto response = co_await client.stream(std::move(request));

  while (co_await response.body.read()) {
  }
}

} // namespace

TEST_CASE("http::client: a POST whose idle connection turns out closed as it goes closes the older idle ones",
          "[client]") {
  // Open when checked, closed once the POST is read: no check can see it coming.
  auto const streamed = GENERATE(false, true);
  async::run_loop loop;
  auto late = 0;
  memory_server server{loop.get_executor(),
                       [&](io::memory_stream stream) { return answer_then_count(std::move(stream), late); },
                       0,
                       {},
                       {}};

  auto const sending = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};

    // Two idle connections.
    static_cast<void>(
        co_await async::when_all(client.send(make_request(verb::get, "/")), client.send(make_request(verb::get, "/"))));

    // On the newer: read, and closed.
    auto post = make_request(verb::post, "/", "ping");

    if (streamed) {
      static_cast<void>(co_await quoll::test::code_thrown_by(stream_to_end(client, std::move(post))));
    } else {
      static_cast<void>(co_await quoll::test::code_thrown_by(client.send(std::move(post))));
    }

    // The older is closed, rather than taken.
    co_return (co_await client.send(make_request(verb::get, "/"))).body;
  };

  REQUIRE(quoll::test::run(loop, sending()) == "ok");
  REQUIRE(late == 1);
  REQUIRE(server.connections == 3);
}

TEST_CASE("http::client: an idle connection found closed closes the older idle ones", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  auto served = 0;
  // The first two connections answer once, then close; the next ones serve the router.
  memory_server server{loop.get_executor(),
                       [&](io::memory_stream stream) {
                         return ++served <= 2 ? answer(std::move(stream),
                                                       {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, false)
                                              : quoll::http::server{router}.serve_connection(std::move(stream));
                       },
                       0,
                       {},
                       {}};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    http::client client{memory_connector{target}, "example.com", 80};
    auto [first, second] =
        co_await async::when_all(client.send(make_request(verb::get, "/")), client.send(make_request(verb::get, "/")));
    // Finds the last idle connection closed, closes the other one, and connects.
    auto third = co_await client.send(make_request(verb::get, "/hello"));
    // Reuses the new connection.
    auto fourth = co_await client.send(make_request(verb::get, "/hello"));

    co_return std::vector<std::string>{first.body, second.body, third.body, fourth.body};
  }(server));

  REQUIRE(bodies == std::vector<std::string>{"ok", "ok", "hello", "hello"});
  REQUIRE(server.connections == 3);
}

TEST_CASE("http::client: a response that cannot be read fails, and its connection is dropped", "[client]") {
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

TEST_CASE("http::client: a connection with bytes past its response is not reused", "[client]") {
  std::string const extra = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1";
  auto const result =
      roundtrip(answering({extra, extra}), {make_request(verb::get, "/"), make_request(verb::get, "/")});

  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.connections == 2);
}

TEST_CASE("http::client: concurrent requests open their own connections, kept for the next ones", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  memory_server server{
      loop.get_executor(),
      [&router](io::memory_stream stream) { return quoll::http::server{router}.serve_connection(std::move(stream)); },
      0,
      {},
      {}};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    http::client client{memory_connector{target}, "example.com", 80};
    auto [first, second] = co_await async::when_all(client.send(make_request(verb::get, "/hello")),
                                                    client.send(make_request(verb::put, "/echo", "ping")));
    auto third = co_await client.send(make_request(verb::get, "/hello"));

    co_return std::vector<std::string>{first.body, second.body, third.body};
  }(server));

  REQUIRE(bodies == std::vector<std::string>{"hello", "ping", "hello"});
  REQUIRE(server.connections == 2);
}

TEST_CASE("http::client: what cannot be sent is refused before the task runs", "[client]") {
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
    REQUIRE_THROWS_AS((http::client{memory_connector{server}, std::string{host}, 80}), std::invalid_argument);
  }

  REQUIRE_THROWS_AS((http::client{memory_connector{server}, std::string{"nul\0", 4}, 80}), std::invalid_argument);
  for (std::string_view const host : {"my-host_1.example.com", "192.0.2.1", "2001:DB8::1", "::ffff:192.0.2.1",
                                      "::", "1::", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:192.0.2.1"}) {
    REQUIRE_NOTHROW(http::client{memory_connector{server}, std::string{host}, 80});
  }

  http::client const client{memory_connector{server}, "example.com", 80};
  auto field = make_request(verb::get, "/");

  field.fields.add("X-Split", "a\r\nInjected: b");
  REQUIRE_THROWS_AS(client.send(make_request(verb::get, "http://example.com/")), std::invalid_argument);
  REQUIRE_THROWS_AS(client.send(std::move(field)), std::invalid_argument);
  REQUIRE(server.connections == 0);
}

TEST_CASE("http::client: built from an http URL", "[client]") {
  using quoll::http::detail::parse_url;

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
    CAPTURE(url);
    REQUIRE_THROWS_AS(http::client{url}, std::invalid_argument);
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
    REQUIRE_THROWS_AS(http::client{url}, std::invalid_argument);
  }

  REQUIRE_NOTHROW(http::client{"http://127.0.0.1:8080"});
  REQUIRE_NOTHROW(http::client{"https://127.0.0.1:8443"});
}

TEST_CASE("http::client: an https URL goes through connect_tls, with the options", "[client][tls]") {
  auto const send = [](std::string url, http::client_options options) {
    tls_recording_connector::log = {};

    auto const call = [&]() -> task<int> {
      http::client const client{tls_recording_connector{}, url, options};

      co_return static_cast<int>((co_await client.send(make_request(verb::get, "/"))).status);
    };

    return quoll::test::run(call());
  };

  http::client_options options;

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

TEST_CASE("http::client: an https URL needs a connector that opens TLS connections", "[client][tls]") {
  REQUIRE_THROWS_AS((http::client{plain_connector{}, "https://example.com"}), std::invalid_argument);
  REQUIRE_NOTHROW((http::client{plain_connector{}, "http://example.com"}));
}

TEST_CASE("http::client: braces after a connector and a URL are options, not a port", "[client]") {
  REQUIRE_NOTHROW(http::client{plain_connector{}, "http://example.com", {}});
  REQUIRE_NOTHROW(http::client{plain_connector{}, std::string{"http://example.com"}, {}});
  REQUIRE_THROWS_AS((http::client{plain_connector{}, "example.com", {}}), std::invalid_argument);
}

TEST_CASE("http::client: the shortcuts send the method, target, body and type they say", "[client]") {
  http::router router;

  auto const seeing = [](std::string method) {
    return [method = std::move(method)](http::request request, http::route_match) -> task<http::response> {
      http::response response;

      response.fields.set("X-Seen", method + '|' + request.target + '|' +
                                        std::string{request.fields.find("Content-Type").value_or("none")} + '|' +
                                        std::string{request.fields.find("Content-Length").value_or("none")} + '|' +
                                        request.body + '|');
      co_return response;
    };
  };

  for (auto const &prefix : {std::string{}, std::string{"/v1"}}) {
    auto const at = prefix + "/at";

    router.add(verb::get, at, "get" + prefix, seeing("GET"));
    router.add(verb::head, at, "head" + prefix, seeing("HEAD"));
    router.add(verb::delete_, at, "delete" + prefix, seeing("DELETE"));
    router.add(verb::post, at, "post" + prefix, seeing("POST"));
    router.add(verb::put, at, "put" + prefix, seeing("PUT"));
    router.add(verb::patch, at, "patch" + prefix, seeing("PATCH"));
  }

  async::run_loop loop;
  memory_server server{
      loop.get_executor(),
      [&router](io::memory_stream stream) { return http::server{router}.serve_connection(std::move(stream)); },
      0,
      {},
      {}};

  auto const call = [&]() -> task<std::vector<std::string>> {
    http::client const client{memory_connector{server}, "example.com", 80};
    http::client const under_a_path{memory_connector{server}, "http://example.com/v1"};
    std::vector<std::string> seen;

    auto const keep = [&seen](http::response const &answer) {
      seen.emplace_back(answer.fields.find("X-Seen").value_or("nothing"));
    };

    keep(co_await client.get("/at?x=1"));
    keep(co_await client.head("/at"));
    keep(co_await client.delete_("/at"));
    keep(co_await client.post("/at", "a", "text/plain"));
    keep(co_await client.put("/at", "b", "application/json"));
    keep(co_await client.patch("/at", "c"));
    keep(co_await client.post("/at", ""));
    keep(co_await under_a_path.get("/at"));
    co_return seen;
  };

  auto const seen = quoll::test::run(loop, call());

  if (server.failure) {
    std::rethrow_exception(server.failure);
  }

  REQUIRE(seen.value() == std::vector<std::string>{"GET|/at?x=1|none|none||", "HEAD|/at|none|none||",
                                                   "DELETE|/at|none|none||", "POST|/at|text/plain|1|a|",
                                                   "PUT|/at|application/json|1|b|", "PATCH|/at|none|1|c|",
                                                   "POST|/at|none|0||", "GET|/v1/at|none|none||"});
}

TEST_CASE("http::client: a shortcut rejects what send() does, before any task", "[client]") {
  http::client const client{plain_connector{}, "example.com", 80};

  REQUIRE_THROWS_AS((void)client.get("no-slash"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.head("no-slash"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.delete_("no-slash"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.post("no-slash", "body"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.put("no-slash", "body"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.patch("no-slash", "body"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.get("/with a space"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.post("/a", "body", "text/plain\r\nEvil: 1"), std::invalid_argument);
}

TEST_CASE("http::client: one moved from still sends", "[client]") {
  auto const call = []() -> task<int> {
    http::client moved_from{tls_recording_connector{}, "http://example.com"};
    http::client const other{std::move(moved_from)};

    // NOLINTNEXTLINE(bugprone-use-after-move)
    co_return static_cast<int>((co_await moved_from.send(make_request(verb::get, "/"))).status);
  };

  REQUIRE(quoll::test::run(call()) == 200);
}

TEST_CASE("http::client: a host takes a port in range", "[client]") {
  static_assert(!std::constructible_from<http::client, plain_connector, std::string, bool>);
  static_assert(!std::constructible_from<http::client, plain_connector, std::string, char>);

  REQUIRE_NOTHROW(http::client{plain_connector{}, "example.com", 65'535});
  REQUIRE_NOTHROW(http::client{plain_connector{}, "example.com", std::uint16_t{80}});
  REQUIRE_THROWS_AS((http::client{plain_connector{}, "example.com", 65'536}), std::invalid_argument);
  REQUIRE_THROWS_AS((http::client{plain_connector{}, "example.com", -1}), std::invalid_argument);
}

#if !defined(__EMSCRIPTEN__)
TEST_CASE("http::client: natively, a URL relative to a page is refused", "[client]") {
  REQUIRE_THROWS_AS(http::client{""}, std::invalid_argument);
  REQUIRE_THROWS_AS(http::client{"/api"}, std::invalid_argument);
  REQUIRE_THROWS_AS(http::client{"."}, std::invalid_argument);
}
#endif

TEST_CASE("http::client: the default connector needs an executor offering a network", "[client]") {
  http::client const client{"http://127.0.0.1:8080"};

  REQUIRE_THROWS_AS(quoll::test::run(client.send(make_request(verb::get, "/"))), std::logic_error);
}

TEST_CASE("http::client: a connection that takes too long fails with timed_out", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 2s})) == std::errc::timed_out);
  REQUIRE(time.now() == 2s);
  REQUIRE(network.connections == 0);
}

TEST_CASE("http::client: a response that takes too long fails with timed_out", "[client][timer]") {
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

namespace {

// A memory_server with the router, counting what is open at once.
struct counted_server {
  explicit counted_server(async::run_loop::executor executor)
      : server{executor,
               [this](io::memory_stream stream) -> task<> {
                 most_open = std::max(most_open, ++open);
                 co_await quoll::http::server{router}.serve_connection(std::move(stream));
                 --open;
               },
               0,
               {},
               {}} {}

  http::router router = make_router();
  int open = 0;
  int most_open = 0;
  memory_server server;
};

http::client_options at_most(std::size_t connections) {
  http::client_options options;

  options.max_connections = connections;
  return options;
}

} // namespace

TEST_CASE("http::client: past max_connections, a request waits for one to be free", "[client]") {
  async::run_loop loop;
  counted_server counted{loop.get_executor()};
  auto const limit = GENERATE(std::size_t{1}, std::size_t{2});
  auto const bodies =
      quoll::test::run(loop, [](memory_server &target, std::size_t places) -> task<std::vector<std::string>> {
        http::client client{memory_connector{target}, "example.com", 80, at_most(places)};
        auto [first, second, third] = co_await async::when_all(client.send(make_request(verb::get, "/hello")),
                                                               client.send(make_request(verb::get, "/hello")),
                                                               client.send(make_request(verb::get, "/hello")));

        co_return std::vector<std::string>{first.body, second.body, third.body};
      }(counted.server, limit));

  REQUIRE(bodies == std::vector<std::string>{"hello", "hello", "hello"});
  REQUIRE(static_cast<std::size_t>(counted.most_open) <= limit);
  REQUIRE(static_cast<std::size_t>(counted.server.connections) <= limit);
}

TEST_CASE("http::client: an idle connection makes room for a new one at the limit", "[client]") {
  async::run_loop loop;
  counted_server counted{loop.get_executor()};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    http::client client{memory_connector{target}, "example.com", 80, at_most(1)};
    auto first = co_await client.send(make_request(verb::get, "/hello"));
    // A body in pieces opens its own connection: the idle one goes first.
    auto second = co_await client.send(upload_of(pieces_of(4, 1)));

    co_await async::yield();
    co_return std::vector<std::string>{first.body, second.body};
  }(counted.server));

  REQUIRE(bodies == std::vector<std::string>{"hello", "xxxx"});
  REQUIRE(counted.server.connections == 2);
  REQUIRE(counted.most_open == 1);
}

TEST_CASE("http::client: only what is idle past the limit makes room", "[client]") {
  async::run_loop loop;
  counted_server counted{loop.get_executor()};
  auto const bodies = quoll::test::run(loop, [](memory_server &target) -> task<std::vector<std::string>> {
    http::client client{memory_connector{target}, "example.com", 80, at_most(2)};
    auto [first, second] = co_await async::when_all(client.send(make_request(verb::get, "/hello")),
                                                    client.send(make_request(verb::get, "/hello")));
    // Two idle, one more to open for a body in pieces: one of them goes, and the other is reused after it.
    auto third = co_await client.send(upload_of(pieces_of(4, 1)));
    auto fourth = co_await client.send(make_request(verb::get, "/hello"));

    co_await async::yield();
    co_return std::vector<std::string>{first.body, second.body, third.body, fourth.body};
  }(counted.server));

  REQUIRE(bodies == std::vector<std::string>{"hello", "hello", "xxxx", "hello"});
  REQUIRE(counted.server.connections == 3);
  REQUIRE(counted.most_open == 2);
}

TEST_CASE("http::client: waiting for a free connection is within connect_timeout", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 1h};
  std::chrono::nanoseconds failed_at{};

  auto const send = [&]() -> task<std::error_code> {
    http::client_options options = at_most(1);

    options.connect_timeout = 2s;
    options.response_timeout = 10s;

    http::client const client{slow_connector{network}, "example.com", 80, options};

    auto const failure_of = [&]() -> task<std::error_code> {
      try {
        static_cast<void>(co_await client.send(make_request(verb::get, "/")));
        co_return std::error_code{};
      } catch (std::system_error const &failure) {
        failed_at = std::min(failed_at == 0s ? time.now() : failed_at, time.now());
        co_return failure.code();
      }
    };

    auto [held, waited] = co_await async::when_all(failure_of(), failure_of());

    co_return waited;
  };

  REQUIRE(quoll::test::run(time, send()) == std::errc::timed_out);
  REQUIRE(failed_at == 2s);
  REQUIRE(network.connections == 1);
}

TEST_CASE("http::client: a request that fails gives its place back", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 5s};

  auto const send = [&]() -> task<std::string> {
    http::client_options options = at_most(1);

    options.response_timeout = 1s;

    http::client client{slow_connector{network}, "example.com", 80, options};

    REQUIRE_THROWS_AS(co_await client.send(make_request(verb::get, "/")), std::system_error);

    network.response_delay = 0s;
    co_return (co_await client.send(make_request(verb::get, "/"))).body;
  };

  REQUIRE(quoll::test::run(time, send()) == "ok");
}

TEST_CASE("http::client: a stop ends a wait for a free connection, and the next request goes", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 2s};

  auto const send = [&]() -> task<std::string> {
    http::client client{slow_connector{network}, "example.com", 80, at_most(1)};
    std::stop_source stopping;

    auto const holds = [&]() -> task<std::string> {
      co_return (co_await client.send(make_request(verb::get, "/"))).body;
    };

    auto const waits = [&]() -> task<std::string> {
      try {
        static_cast<void>(co_await async::detail::stop_scope<http::response>{client.send(make_request(verb::get, "/")),
                                                                             stopping.get_token()});
        co_return "answered";
      } catch (std::system_error const &failure) {
        co_return failure.code() == std::errc::operation_canceled ? "stopped" : "failed";
      }
    };

    auto const stops = [&]() -> task<std::string> {
      co_await async::sleep_for(1s);
      stopping.request_stop();
      co_return "";
    };

    auto [held, waited, stopped] = co_await async::when_all(holds(), waits(), stops());
    auto next = co_await client.send(make_request(verb::get, "/"));

    co_return held + " " + waited + " " + next.body;
  };

  REQUIRE(quoll::test::run(time, send()) == "ok stopped ok");
}

namespace {

// What a server answers, raw, whichever connection a request comes on, and the requests it read.
struct script {
  std::vector<std::string> replies;
  std::size_t next = 0;
  std::vector<http::request> seen{};
};

task<> play(io::memory_stream stream, script &played) {
  std::string buffer;

  while (played.next < played.replies.size()) {
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    played.seen.push_back(parser.release());

    auto const &reply = played.replies[played.next++];

    (void)co_await io::write(stream, std::as_bytes(std::span{reply}));
  }
}

std::string redirect_to(int status, std::string_view location) {
  return "HTTP/1.1 " + std::to_string(status) + " Moved\r\nLocation: " + std::string{location} +
         "\r\nContent-Length: 0\r\n\r\n";
}

constexpr std::string_view reply_done = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone";

http::client_options following(std::size_t redirects) {
  http::client_options options;

  options.max_redirects = redirects;
  return options;
}

struct followed {
  outcome result;
  std::vector<http::request> seen;
};

followed follow(std::vector<std::string> replies, http::request request, http::client_options options) {
  script played{std::move(replies)};
  auto result = roundtrip([&played](io::memory_stream stream) { return play(std::move(stream), played); },
                          {std::move(request)}, {}, options);

  return {std::move(result), std::move(played.seen)};
}

} // namespace

TEST_CASE("http::client: a redirect is a result unless max_redirects says otherwise", "[client]") {
  auto const out = follow({redirect_to(302, "/next")}, make_request(verb::get, "/start"), following(0));

  REQUIRE(out.result.responses.size() == 1);
  REQUIRE(out.result.responses[0].status == http::status::found);
  REQUIRE(out.seen.size() == 1);
}

TEST_CASE("http::client: a redirect sends the request again, on the connection it kept", "[client]") {
  auto request = make_request(verb::get, "/a/start?x=1");

  request.fields.add("Authorization", "Bearer token");

  auto const out =
      follow({redirect_to(302, "next?y=2#top"), std::string{reply_done}}, std::move(request), following(1));

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.size() == 1);
  REQUIRE(out.result.responses[0].body == "done");
  REQUIRE(out.result.connections == 1);
  REQUIRE(out.seen.size() == 2);
  REQUIRE(out.seen[1].target == "/a/next?y=2");
  REQUIRE(out.seen[1].fields.find("Authorization") == "Bearer token");
  REQUIRE(out.seen[1].fields.find("Host") == "example.com");
}

TEST_CASE("http::client: what a redirect does to the method and the body", "[client]") {
  struct expected {
    int status;
    verb method;
    verb next;
  };

  auto const [status, method, next] = GENERATE(
      expected{301, verb::post, verb::get}, expected{302, verb::post, verb::get}, expected{303, verb::post, verb::get},
      expected{301, verb::put, verb::put}, expected{302, verb::put, verb::put}, expected{303, verb::put, verb::get},
      expected{303, verb::delete_, verb::get}, expected{303, verb::head, verb::head},
      expected{301, verb::head, verb::head}, expected{307, verb::post, verb::post},
      expected{308, verb::post, verb::post}, expected{307, verb::put, verb::put}, expected{307, verb::head, verb::head},
      expected{308, verb::get, verb::get}, expected{303, verb::get, verb::get});
  auto request = make_request(method, "/start", method == verb::head ? "" : "ping");

  request.fields.add("Content-Type", "text/plain");
  request.fields.add("CONTENT-LANGUAGE", "fr");
  request.fields.add("Expect", "100-continue");
  request.fields.add("Digest", "sha-256=abc");
  request.fields.add("ETag", "\"v1\"");
  request.fields.add("If-Match", "\"v1\"");
  request.fields.add("Range", "bytes=0-1");
  request.fields.add("Last-Modified", "Thu, 05 Nov 2026 10:00:00 GMT");
  request.fields.add("X-Kept", "yes");

  std::vector<std::string> replies;

  replies.push_back(redirect_to(status, "/next"));
  // A HEAD reads no body: what it gets must not have one to read past.
  replies.emplace_back(next == verb::head ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n" : reply_done);

  auto const out = follow(std::move(replies), std::move(request), following(1));

  CAPTURE(status, to_string(method));
  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.size() == 1);
  REQUIRE(out.result.responses[0].status == http::status::ok);
  REQUIRE(out.seen.size() == 2);
  REQUIRE(out.seen[1].method == next);
  REQUIRE(out.seen[1].target == "/next");
  REQUIRE(out.seen[1].fields.find("X-Kept") == "yes");

  if (next != method) {
    REQUIRE(out.seen[1].body.empty());
    REQUIRE(!out.seen[1].fields.contains("Content-Type"));
    REQUIRE(!out.seen[1].fields.contains("Content-Language"));
    REQUIRE(!out.seen[1].fields.contains("Content-Length"));
    REQUIRE(!out.seen[1].fields.contains("Expect"));
    REQUIRE(!out.seen[1].fields.contains("Digest"));
    REQUIRE(!out.seen[1].fields.contains("ETag"));
    REQUIRE(!out.seen[1].fields.contains("If-Match"));
    REQUIRE(!out.seen[1].fields.contains("Range"));
    REQUIRE(!out.seen[1].fields.contains("Last-Modified"));
  } else {
    REQUIRE(out.seen[1].body == out.seen[0].body);
    REQUIRE(out.seen[1].fields.find("Content-Type") == "text/plain");
    REQUIRE(out.seen[1].fields.find("If-Match") == "\"v1\"");
    REQUIRE(out.seen[1].fields.contains("Expect"));
  }
}

TEST_CASE("http::client: a chunked body and its trailers follow the method through a redirect", "[client]") {
  auto const status = GENERATE(303, 307);
  auto request = make_request(verb::post, "/start", "ping");

  request.fields.add("Transfer-Encoding", "chunked");
  request.fields.add("Trailer", "X-Sum");
  request.trailers.add("X-Sum", "1");

  auto const out = follow({redirect_to(status, "/next"), std::string{reply_done}}, std::move(request), following(1));

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.at(0).body == "done");
  REQUIRE(out.seen.size() == 2);
  REQUIRE(out.seen[0].trailers.find("X-Sum") == "1");

  if (status == 303) {
    REQUIRE(out.seen[1].method == verb::get);
    REQUIRE(out.seen[1].body.empty());
    REQUIRE(out.seen[1].trailers.empty());
    REQUIRE(!out.seen[1].fields.contains("Transfer-Encoding"));
    REQUIRE(!out.seen[1].fields.contains("Trailer"));
  } else {
    REQUIRE(out.seen[1].method == verb::post);
    REQUIRE(out.seen[1].body == "ping");
    REQUIRE(out.seen[1].trailers.find("X-Sum") == "1");
  }
}

TEST_CASE("http::client: a redirect closing its connection is followed on a new one", "[client]") {
  auto const out = follow({"HTTP/1.1 302 Found\r\nLocation: /next\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
                           std::string{reply_done}},
                          make_request(verb::get, "/start"), following(1));

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.at(0).body == "done");
  REQUIRE(out.result.connections == 2);
  REQUIRE(out.seen.at(1).target == "/next");
}

TEST_CASE("http::client: the same Location twice is followed", "[client]") {
  auto const out = follow({"HTTP/1.1 302 Found\r\nLocation: /next\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n",
                           std::string{reply_done}},
                          make_request(verb::get, "/start"), following(1));

  REQUIRE(out.result.responses.at(0).body == "done");
  REQUIRE(out.seen.at(1).target == "/next");
}

TEST_CASE("http::client: a POST sent again by a 307 takes the connection kept", "[client]") {
  auto const out = follow({redirect_to(307, "/next"), std::string{reply_done}},
                          make_request(verb::post, "/start", "ping"), following(1));

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.at(0).body == "done");
  REQUIRE(out.result.connections == 1);
  REQUIRE(out.seen.size() == 2);
  REQUIRE(out.seen[1].method == verb::post);
  REQUIRE(out.seen[1].body == "ping");
}

TEST_CASE("http::client: past max_redirects, a request fails", "[client]") {
  auto const redirects = GENERATE(std::size_t{1}, std::size_t{2});
  std::vector const replies(redirects + 2, redirect_to(301, "/again"));
  auto const out = follow(replies, make_request(verb::get, "/start"), following(redirects));

  REQUIRE(out.result.error == http::error::too_many_redirects);
  REQUIRE(out.result.responses.empty());
  // The first request, and those the limit lets go.
  REQUIRE(out.seen.size() == redirects + 1);
}

TEST_CASE("http::client: a response that cannot be followed is the result", "[client]") {
  auto const reply = GENERATE(
      // Another server or scheme.
      redirect_to(302, "http://other.example/next"), redirect_to(302, "https://example.com/next"),
      redirect_to(302, "//other.example/next"), redirect_to(302, "http://example.com:8080/next"),
      redirect_to(302, "http://example.com@other.example/next"),
      redirect_to(302, "http://example.com:80@other.example/"), redirect_to(302, "ftp://example.com/next"),
      redirect_to(302, "mailto:someone@example.com"),
      // What says nothing, or differs.
      redirect_to(302, ""), redirect_to(302, "#top"),
      std::string{"HTTP/1.1 302 Found\r\nLocation: /a\r\nLocation: /b\r\nContent-Length: 0\r\n\r\n"},
      // What a proxy or a framework would read as another server.
      redirect_to(302, "/..//other.example/next"), redirect_to(302, "/.//other.example/next"),
      redirect_to(302, "http://example.com//other.example/next"),
      // What cannot be read.
      redirect_to(302, "http:\\\\other.example\\next"), redirect_to(302, "/\\other.example"), redirect_to(302, "/a b"),
      redirect_to(302, "/%zz"), redirect_to(302, "/caf\xc3\xa9"),
      // No field, or a status that is no redirect.
      std::string{"HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n"}, redirect_to(300, "/next"),
      redirect_to(304, "/next"), redirect_to(200, "/next"));

  auto const out = follow({reply}, make_request(verb::get, "/start"), following(3));

  CAPTURE(reply);
  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.size() == 1);
  REQUIRE(out.seen.size() == 1);
}

TEST_CASE("http::client: the last redirect leaving the server is the result, not an error", "[client]") {
  auto const out = follow({redirect_to(302, "/next"), redirect_to(302, "http://other.example/")},
                          make_request(verb::get, "/start"), following(1));

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.size() == 1);
  REQUIRE(out.result.responses[0].status == http::status::found);
}

TEST_CASE("http::client: a redirect keeps within max_connections", "[client]") {
  auto options = at_most(1);

  options.max_redirects = 1;

  // The place goes back between two requests of a chain, and the GET takes the connection the POST kept.
  auto const out =
      follow({redirect_to(303, "/next"), std::string{reply_done}}, make_request(verb::post, "/start", "ping"), options);

  REQUIRE(!out.result.error);
  REQUIRE(out.result.responses.at(0).body == "done");
  REQUIRE(out.result.connections == 1);
}

namespace {

// What a client built from a URL connects with, which it default-constructs.
struct url_connector {
  static inline memory_server *server = nullptr;
  static inline int tls_connections = 0;

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    co_return co_await memory_connector{*server}.connect(std::move(host), port);
  }

  [[nodiscard]] task<io::any_stream> connect_tls(std::string host, std::uint16_t port,
                                                 io::tls_client_options const &) const {
    ++tls_connections;
    co_return co_await connect(std::move(host), port);
  }
};

static_assert(io::TlsConnector<url_connector>);

} // namespace

TEST_CASE("http::client: a redirect goes where it says, whatever the path of the client's URL", "[client]") {
  async::run_loop loop;
  script played{{redirect_to(302, "next"), redirect_to(302, "/outside"), std::string{reply_done}}};
  memory_server server{
      loop.get_executor(), [&played](io::memory_stream stream) { return play(std::move(stream), played); }, 0, {}, {}};

  url_connector::server = &server;

  auto const body = quoll::test::run(loop, []() -> task<std::string> {
    http::client const client{url_connector{}, "http://example.com/v1", following(2)};

    co_return (co_await client.send(make_request(verb::get, "/start"))).body;
  }());

  REQUIRE(body == "done");
  REQUIRE(played.seen.size() == 3);
  REQUIRE(played.seen[0].target == "/v1/start");
  REQUIRE(played.seen[1].target == "/v1/next");
  REQUIRE(played.seen[2].target == "/outside");
}

TEST_CASE("http::client: a chain over https stays on https, on TLS connections", "[client][tls]") {
  async::run_loop loop;
  script played{
      {"HTTP/1.1 302 Found\r\nLocation: https://example.com/next\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
       redirect_to(302, "http://example.com/plain")}};
  memory_server server{
      loop.get_executor(), [&played](io::memory_stream stream) { return play(std::move(stream), played); }, 0, {}, {}};

  url_connector::server = &server;
  url_connector::tls_connections = 0;

  auto const status = quoll::test::run(loop, []() -> task<http::status> {
    http::client const client{url_connector{}, "https://example.com", following(3)};

    co_return (co_await client.send(make_request(verb::get, "/start"))).status;
  }());

  // The second redirect would leave https: it is the result.
  REQUIRE(status == http::status::found);
  REQUIRE(played.seen.size() == 2);
  REQUIRE(played.seen[1].target == "/next");
  REQUIRE(played.seen[1].fields.find("Host") == "example.com");
  REQUIRE(url_connector::tls_connections == 2);
  REQUIRE(server.destination == "example.com:443");
}

TEST_CASE("http::client: the Host field of a request says what a Location names", "[client]") {
  auto const with_host = [] {
    auto request = make_request(verb::get, "/start");

    request.fields.add("Host", "tenant.example");
    return request;
  };

  auto const named =
      follow({redirect_to(302, "http://tenant.example/next"), std::string{reply_done}}, with_host(), following(1));

  REQUIRE(named.result.responses.at(0).body == "done");
  REQUIRE(named.seen.at(1).target == "/next");
  REQUIRE(named.seen.at(1).fields.find("Host") == "tenant.example");

  auto const relative = follow({redirect_to(302, "/next"), std::string{reply_done}}, with_host(), following(1));

  REQUIRE(relative.result.responses.at(0).body == "done");
  REQUIRE(relative.seen.at(1).fields.find("Host") == "tenant.example");

  // The server the client connects to is not what this request asked for.
  auto const other = follow({redirect_to(302, "http://example.com/next")}, with_host(), following(1));

  REQUIRE(other.result.responses.at(0).status == http::status::found);
  REQUIRE(other.seen.size() == 1);
}

TEST_CASE("http::client: a stop ends a chain on a later request", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 2s, .first_replies = {redirect_to(302, "/next")}};
  std::stop_source stopping;
  std::chrono::nanoseconds ended{};

  auto const send = [&]() -> task<std::string> {
    http::client client{slow_connector{network}, "example.com", 80, following(1)};

    auto const sends = [&]() -> task<std::string> {
      try {
        static_cast<void>(co_await async::detail::stop_scope<http::response>{client.send(make_request(verb::get, "/")),
                                                                             stopping.get_token()});
        co_return "answered";
      } catch (std::system_error const &failure) {
        ended = time.now();
        co_return failure.code() == std::errc::operation_canceled ? "stopped" : "failed";
      }
    };

    auto const stops = [&]() -> task<std::string> {
      co_await async::sleep_for(3s);
      stopping.request_stop();
      co_return "";
    };

    auto [sent, stopped] = co_await async::when_all(sends(), stops());

    co_return sent + stopped;
  };

  REQUIRE(quoll::test::run(time, send()) == "stopped");
  REQUIRE(ended == 3s);
}

TEST_CASE("http::client: each request of a chain has its own response_timeout", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 2s, .first_replies = {redirect_to(302, "/next")}};

  REQUIRE(quoll::test::run(time, send_gets(network, {.max_redirects = 1, .response_timeout = 3s})) ==
          std::error_code{});
  REQUIRE(time.now() == 4s);
  REQUIRE(network.connections == 1);
}

TEST_CASE("http::client: where a Location leads", "[client]") {
  using quoll::http::detail::resolve_location;

  auto const resolve = [](std::string_view location, std::string_view target = "/b/c/d?q") {
    return resolve_location(location, "Example.com", false, target);
  };

  // The examples of RFC 3986, 5.4, that stay on the server, from its /b/c/d?q.
  REQUIRE(resolve("g") == "/b/c/g");
  REQUIRE(resolve("./g") == "/b/c/g");
  REQUIRE(resolve("g/") == "/b/c/g/");
  REQUIRE(resolve("/g") == "/g");
  REQUIRE(resolve("?y") == "/b/c/d?y");
  REQUIRE(resolve("g?y") == "/b/c/g?y");
  REQUIRE(resolve("g#s") == "/b/c/g");
  REQUIRE(resolve("g?y#s") == "/b/c/g?y");
  REQUIRE(resolve(".") == "/b/c/");
  REQUIRE(resolve("./") == "/b/c/");
  REQUIRE(resolve("..") == "/b/");
  REQUIRE(resolve("../") == "/b/");
  REQUIRE(resolve("../g") == "/b/g");
  REQUIRE(resolve("../..") == "/");
  REQUIRE(resolve("../../") == "/");
  REQUIRE(resolve("../../g") == "/g");
  REQUIRE(resolve("../../../g") == "/g");
  REQUIRE(resolve("../../../../g") == "/g");
  REQUIRE(resolve("/./g") == "/g");
  REQUIRE(resolve("/../g") == "/g");
  REQUIRE(resolve("g.") == "/b/c/g.");
  REQUIRE(resolve(".g") == "/b/c/.g");
  REQUIRE(resolve("g..") == "/b/c/g..");
  REQUIRE(resolve("..g") == "/b/c/..g");
  REQUIRE(resolve("./../g") == "/b/g");
  REQUIRE(resolve("./g/.") == "/b/c/g/");
  REQUIRE(resolve("g/./h") == "/b/c/g/h");
  REQUIRE(resolve("g/../h") == "/b/c/h");
  REQUIRE(resolve("g?y/./x") == "/b/c/g?y/./x");

  // A full URL naming this server.
  REQUIRE(resolve("http://example.com/g") == "/g");
  REQUIRE(resolve("HTTP://EXAMPLE.COM:80/g?y") == "/g?y");
  REQUIRE(resolve("http://example.com") == "/");
  REQUIRE(resolve("http://example.com?y") == "/?y");
  REQUIRE(resolve("//example.com/g") == "/g");
  REQUIRE(resolve("/a//b") == "/a//b");
  REQUIRE(resolve("/g?a[]=1") == "/g?a[]=1");
  REQUIRE(resolve("/%2e%2e/g") == "/%2e%2e/g");
  REQUIRE(resolve("/a%2Fb") == "/a%2Fb");
  // A target's path, the client's URL's included, is one like any other.
  REQUIRE(resolve("next", "/v1/users") == "/v1/next");
  REQUIRE(resolve("next", "/a/b#frag/c?x") == "/a/next");
  REQUIRE(resolve("?y", "/a#frag") == "/a?y");
  REQUIRE(resolve("?y", "/a/./b") == "/a/./b?y");
  REQUIRE(resolve("?y", "/a/../b?x") == "/a/../b?y");
  // A Host field that cannot be read names nothing, and a relative reference needs none.
  REQUIRE(!resolve_location("http://example.com/g", "exa mple:x", false, "/"));
  REQUIRE(resolve_location("/g", "exa mple:x", false, "/") == "/g");
  REQUIRE(resolve("/next", "/v1/users") == "/next");

  // On another port, and over TLS.
  REQUIRE(resolve_location("http://example.com:8080/g", "example.com:8080", false, "/") == "/g");
  REQUIRE(resolve_location("http://example.com:08080/g", "example.com:8080", false, "/") == "/g");
  REQUIRE(resolve_location("https://example.com/g", "example.com", true, "/") == "/g");
  REQUIRE(resolve_location("//example.com:443/g", "example.com", true, "/") == "/g");
  REQUIRE(resolve_location("http://[::1]:8080/g", "[::1]:8080", false, "/") == "/g");
  REQUIRE(resolve_location("http://[::1]/g", "[::1]", false, "/") == "/g");
  REQUIRE(!resolve_location("https://example.com/g", "example.com", false, "/"));
  REQUIRE(!resolve_location("http://example.com/g", "example.com", true, "/"));
  REQUIRE(!resolve_location("http://example.com/g", "example.com:8080", false, "/"));
  REQUIRE(!resolve_location("http://[::1", "[::1]", false, "/"));
  REQUIRE(!resolve_location("http://[::1]x/g", "[::1]", false, "/"));
  REQUIRE(!resolve_location("http://example.com:65616/g", "example.com", false, "/"));
  REQUIRE(!resolve_location("http://example.com:000080/g", "example.com", false, "/"));

  for (std::string_view const refused : {"",
                                         "/a[b]/c",
                                         "/%2F/other.example/g",
                                         "/%2e//other.example/g",
                                         "/%2e%2e//other.example",
                                         "/%09/other.example",
                                         "/%2e/%2fother.example",
                                         "/%5C/other.example",
                                         "/%2f%2fother.example",
                                         "/%5Cother.example",
                                         "/a%00b",
                                         "#s",
                                         "http://example.com:",
                                         "http://[example.com]/g",
                                         "/..//g",
                                         "/.//g",
                                         "http://example.com//g",
                                         "http://other.example/g",
                                         "//other.example/g",
                                         "///g",
                                         "http:g",
                                         "http:/g",
                                         ":g",
                                         "javascript:alert(1)",
                                         "http://user@example.com/g",
                                         "http://example.com@other.example/g",
                                         "http://example.com:80@other.example/g",
                                         "http://example.com:x/g",
                                         "http://example.com.other.example/g",
                                         "\\\\other.example\\g",
                                         "/\\other.example",
                                         "/a b",
                                         "/a\tb",
                                         "/a\"b",
                                         "/%zz",
                                         "/%2",
                                         "/caf\xc3\xa9"}) {
    CAPTURE(refused);
    REQUIRE(!resolve(refused));
  }
}

TEST_CASE("http::client: requests that end in time succeed, on one connection", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1s, .response_delay = 1s};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 2s, .response_timeout = 2s}, 3)) ==
          std::error_code{});
  REQUIRE(time.now() == 4s);
  REQUIRE(network.connections == 1);
}

TEST_CASE("http::client: a request timing out on a kept connection is not sent again, nor the connection reused",
          "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .response_delay = 1s};

  std::error_code error;

  auto const requests = [&]() -> task<int> {
    http::client const client{slow_connector{network}, "server", 80, {.response_timeout = 2s}};

    (void)co_await client.send(get_root());
    network.response_delay = 1h;

    try {
      (void)co_await client.send(get_root());
    } catch (std::system_error const &failure) {
      error = failure.code();
    }

    auto const after_timeout = network.connections;

    network.response_delay = 1s;
    (void)co_await client.send(get_root());
    co_return after_timeout;
  };

  REQUIRE(quoll::test::run(time, requests()) == 1);
  REQUIRE(error == std::errc::timed_out);
  REQUIRE(network.connections == 2);
}

TEST_CASE("http::client: the default timeouts apply", "[client][timer]") {
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

TEST_CASE("http::client: a negative timeout has expired, rather than lifting the limit", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = -1s})) == std::errc::timed_out);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("http::client: a timeout of zero does not apply", "[client][timer]") {
  virtual_time time;
  slow_network network{.time = &time, .connect_delay = 1h, .response_delay = 1h};

  REQUIRE(quoll::test::run(time, send_gets(network, {.connect_timeout = 0s, .response_timeout = 0s})) ==
          std::error_code{});
  REQUIRE(time.now() == 2h);
}

TEST_CASE("http::client: a stop request is no timeout", "[client][timer]") {
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

namespace {

// What a stream() gave, read to its end.
struct read_whole {
  http::status status = http::status::ok;
  std::string body;
  std::size_t largest = 0;
  std::string trailer;
  std::error_code error;
  int connections = 0;
};

http::router make_streaming_router() {
  http::router router;

  router.get("/pieces", [](http::request) {
    http::response response;

    response.fields.add("X-Head", "yes");
    response.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{4} * 1024, 'x');

      for (auto index = 0; index < 256; ++index) {
        co_await out.write(piece);
      }
    };
    return response;
  });
  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  router.get("/empty", [](http::request) { return http::response{}; });
  return router;
}

task<read_whole> stream_all(memory_server &server, std::string target, http::client_options options = {},
                            verb method = verb::get) {
  http::client const client{memory_connector{server}, "example.com", 80, std::move(options)};
  read_whole out;

  try {
    auto response = co_await client.stream(make_request(method, std::move(target)));

    out.status = response.status;

    while (auto const piece = co_await response.body.read()) {
      out.body += *piece;
      out.largest = std::max(out.largest, piece->size());
    }

    out.trailer = response.trailers->find("X-Sum").value_or("");
  } catch (std::system_error const &failure) {
    out.error = failure.code();
  }

  co_return out;
}

read_whole stream_from(std::function<task<>(io::memory_stream)> serve, std::string target,
                       http::client_options options = {}, verb method = verb::get) {
  async::run_loop loop;
  memory_server server{loop.get_executor(), std::move(serve), 0, {}, {}};
  auto out = quoll::test::run(loop, stream_all(server, std::move(target), std::move(options), method));

  REQUIRE(out);
  out->connections = server.connections;
  return std::move(*out);
}

} // namespace

TEST_CASE("http::client: a response read in pieces, past what memory would hold", "[client]") {
  auto const router = make_streaming_router();
  http::client_options options;

  // Far less than the body: this limit is for what is read whole.
  options.limits.max_body_size = 16;

  auto const got = stream_from(serving(router), "/pieces", options);

  REQUIRE(!got.error);
  REQUIRE(got.status == http::status::ok);
  REQUIRE(got.body == std::string(std::size_t{1024} * 1024, 'x'));
  // What a read of the stream brings, rather than the body.
  REQUIRE(got.largest <= 8192);
}

TEST_CASE("http::client: stream() takes bodies of every size", "[client]") {
  auto const router = make_streaming_router();

  SECTION("a whole body") {
    auto const got = stream_from(serving(router), "/hello");

    REQUIRE(got.body == "hello");
  }

  SECTION("a body up to the end of the stream") {
    auto const got = stream_from(answering({"HTTP/1.1 200 OK\r\n\r\nping"}), "/");

    REQUIRE(!got.error);
    REQUIRE(got.body == "ping");
  }

  SECTION("no body") {
    auto const got = stream_from(serving(router), "/empty");

    REQUIRE(!got.error);
    REQUIRE(got.body.empty());
  }

  SECTION("a response to HEAD") {
    auto const got = stream_from(serving(router), "/hello", {}, verb::head);

    REQUIRE(!got.error);
    REQUIRE(got.body.empty());
  }

  SECTION("a route that is not there is a result") {
    auto const got = stream_from(serving(router), "/nowhere");

    REQUIRE(!got.error);
    REQUIRE(got.status == http::status::not_found);
  }
}

TEST_CASE("http::client: a body read to its end gives its connection back, and its trailers", "[client]") {
  async::run_loop loop;
  auto const replies = std::vector<std::string>{
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nping\r\n0\r\nX-Sum: 1\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong"};
  memory_server server{loop.get_executor(), answering(replies), 0, {}, {}};

  auto const reading = [&]() -> task<std::vector<std::string>> {
    http::client const client{memory_connector{server}, "example.com", 80};
    std::vector<std::string> seen;

    for (auto attempt = 0; attempt < 2; ++attempt) {
      auto response = co_await client.stream(make_request(verb::get, "/"));
      std::string body;

      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }

      // It goes on saying so.
      REQUIRE_FALSE(co_await response.body.read());
      seen.push_back(body);
      seen.emplace_back(response.trailers->find("X-Sum").value_or("none"));
    }

    co_return seen;
  };

  auto const seen = quoll::test::run(loop, reading());

  REQUIRE(seen);
  REQUIRE(*seen == std::vector<std::string>{"ping", "1", "pong", "none"});
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: a stream given up on closes its connection", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client_options options;

    // What it held must come back, or what follows waits for ever.
    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};

    {
      auto response = co_await client.stream(make_request(verb::get, "/pieces"));

      // A piece, and no more.
      static_cast<void>(co_await response.body.read());
    }

    co_return (co_await client.send(make_request(verb::get, "/hello"))).body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello");
  REQUIRE(server.connections == 2);
}

TEST_CASE("http::client: a body in pieces fails as send() would", "[client]") {
  SECTION("cut short, after what came") {
    auto const got = stream_from(answering({"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nping"}), "/");

    REQUIRE(got.body == "ping");
    REQUIRE(got.error == http::error::partial_message);
  }

  SECTION("past its limit") {
    http::client_options options;

    options.limits.max_streamed_body_size = 2;

    auto const got = stream_from(answering({"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nping"}), "/", options);

    REQUIRE(got.error == http::error::body_too_large);
  }

  SECTION("no response at all") {
    auto const got = stream_from(answering({}), "/");

    REQUIRE(got.error == io::error::end_of_stream);
  }
}

TEST_CASE("http::client: stream() skips interim responses, and reuses what send() kept", "[client]") {
  auto const interim =
      stream_from(answering({"HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"}), "/");

  REQUIRE(interim.status == http::status::ok);
  REQUIRE(interim.body == "hi");

  async::run_loop loop;
  auto const router = make_streaming_router();
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};

    static_cast<void>(co_await client.send(make_request(verb::get, "/hello")));

    auto response = co_await client.stream(make_request(verb::get, "/hello"));
    std::string body;

    while (auto const piece = co_await response.body.read()) {
      body += *piece;
    }

    co_return body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello");
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: a stream holds its place in max_connections until its end", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};
  std::vector<std::string> order;

  auto const reading = [&]() -> task<int> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};

    auto const first = [&]() -> task<int> {
      auto response = co_await client.stream(make_request(verb::get, "/pieces"));

      order.emplace_back("first has its head");

      for (auto turn = 0; turn < 32; ++turn) {
        co_await async::yield();
      }

      while (co_await response.body.read()) {
      }

      order.emplace_back("first ended");
      co_return 1;
    };

    auto const second = [&]() -> task<int> {
      static_cast<void>(co_await client.send(make_request(verb::get, "/hello")));
      order.emplace_back("second answered");
      co_return 2;
    };

    auto [one, two] = co_await async::when_all(first(), second());

    co_return one + two;
  };

  REQUIRE(quoll::test::run(loop, reading()) == 3);
  REQUIRE(order == std::vector<std::string>{"first has its head", "first ended", "second answered"});
}

namespace {

// A connector to a router on a virtual time, whose timeouts then jump.
class timed_connector {
public:
  timed_connector(virtual_time &clock, http::router const &routes) noexcept : clock_(&clock), routes_(&routes) {}

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    co_spawn(clock_->get_executor(), quoll::http::server{*routes_}.serve_connection(std::move(server_end)),
             async::detached);
    co_return io::any_stream{std::move(client_end)};
  }

private:
  virtual_time *clock_;
  http::router const *routes_;
};

} // namespace

TEST_CASE("http::client: a body in pieces has a response timeout per piece", "[client][timer]") {
  virtual_time time;
  http::router router;

  router.get("/slowly", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      for (auto index = 0; index < 3; ++index) {
        co_await async::sleep_for(2s);
        co_await out.write("part");
      }

      co_await async::sleep_for(1h);
    };
    return response;
  });

  auto const reading = [&]() -> task<std::string> {
    http::client const client{timed_connector{time, router}, "example.com", 80, {.response_timeout = 3s}};
    auto response = co_await client.stream(make_request(verb::get, "/slowly"));
    std::string body;

    try {
      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }
    } catch (std::system_error const &failure) {
      body += failure.code() == std::errc::timed_out ? "|timed out" : "|another failure";
    }

    co_return body;
  };

  // Three pieces two seconds apart, past one timeout in all, and the last never comes.
  REQUIRE(quoll::test::run(time, reading()) == "partpartpart|timed out");
}

TEST_CASE("http::client: stream() goes on from an idle connection found closed, afresh", "[client]") {
  auto const second_answer = std::string{"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond"};
  auto const first_answer = std::string{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst"};
  // Goes without a word, or answers 408 first. A connection cut in the middle of an answer is an answer cut, as with
  // send().
  auto const on_idle = GENERATE(as<std::string>{}, "", "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n\r\n");
  auto connection = 0;

  auto const serve = [&](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    ++connection;

    auto const this_one = connection;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    static_cast<void>(parser.release());

    auto const &answer = this_one == 1 ? first_answer : second_answer;

    (void)co_await io::write(stream, std::as_bytes(std::span{answer}));

    if (this_one != 1) {
      co_return;
    }

    // Kept by the client, then gone at its next request.
    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{on_idle}));
  };

  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};

    static_cast<void>(co_await client.send(make_request(verb::get, "/")));

    auto response = co_await client.stream(make_request(verb::get, "/"));
    std::string body;

    while (auto const piece = co_await response.body.read()) {
      body += *piece;
    }

    co_return body;
  };

  CAPTURE(on_idle);
  REQUIRE(quoll::test::run(loop, reading()) == "second");
  REQUIRE(server.connections == 2);
}

TEST_CASE("http::client: stream() skips an interim response with a body of its own, in two reads", "[client]") {
  auto const serve = [](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    std::string_view const head = "HTTP/1.1 150 Odd\r\nContent-Length: 4\r\n\r\n";
    std::string_view const rest = "pingHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));

    for (auto turn = 0; turn < 16; ++turn) {
      co_await async::yield();
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{rest}));
  };

  auto const got = stream_from(serve, "/");

  REQUIRE(!got.error);
  REQUIRE(got.status == http::status::ok);
  REQUIRE(got.body == "hi");
}

TEST_CASE("http::client: a response with nothing to read gives its connection back at once", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};

    // Dropped without reading: there is nothing to read.
    static_cast<void>(co_await client.stream(make_request(verb::head, "/hello")));

    co_return (co_await client.send(make_request(verb::get, "/hello"))).body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello");
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: a cancellation ends the reading, as a stream does not take it up again", "[client][timer]") {
  virtual_time time;

  http::router router;

  router.get("/slowly", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      co_await async::sleep_for(5s);
      co_await out.write("first");
      co_await async::sleep_for(5s);
      co_await out.write("second");
    };
    return response;
  });

  auto const reading = [&]() -> task<std::string> {
    http::client const client{timed_connector{time, router}, "example.com", 80, {.response_timeout = 0s}};
    auto response = co_await client.stream(make_request(verb::get, "/slowly"));
    std::string seen = std::string{*co_await response.body.read()};

    // Its own timeout, around a read: the body is not at fault.
    try {
      static_cast<void>(co_await async::with_timeout(1s, response.body.read()));
      seen += "|no timeout";
    } catch (std::system_error const &thrown) {
      seen += thrown.code() == std::errc::timed_out ? "|gave up" : "|another failure";
    }

    try {
      static_cast<void>(co_await response.body.read());
      seen += "|read again";
    } catch (std::system_error const &) {
      seen += "|ended";
    }

    co_return seen;
  };

  REQUIRE(quoll::test::run(time, reading()) == "first|gave up|ended");
}

TEST_CASE("http::client: a body that failed leaves its place to others", "[client]") {
  auto connections = 0;

  auto const serve = [&](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    ++connections;

    auto const this_one = connections;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    // Cut short on the first, whole on the next.
    auto const answer = std::string{this_one == 1 ? "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nping"
                                                  : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"};

    (void)co_await io::write(stream, std::as_bytes(std::span{answer}));
  };

  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};
    auto response = co_await client.stream(make_request(verb::get, "/"));

    try {
      while (co_await response.body.read()) {
      }
    } catch (std::system_error const &) {
    }

    // With `response` still here: this would wait for its place for ever.
    co_return (co_await client.send(make_request(verb::get, "/"))).body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hi");
}

TEST_CASE("http::client: what comes of a body does not depend on how it was cut", "[client]") {
  std::string const wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nping\r\nZ\r\n";
  // In one read, past the head, and in the middle of the good chunk.
  auto const cut = GENERATE_COPY(wire.size(), wire.find("4\r\n"), wire.find("ng\r\n"));

  auto const serve = [&](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    auto const first = std::string_view{wire}.substr(0, cut);
    auto const rest = std::string_view{wire}.substr(cut);

    (void)co_await io::write(stream, std::as_bytes(std::span{first}));

    for (auto turn = 0; turn < 16; ++turn) {
      co_await async::yield();
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{rest}));
  };

  auto const got = stream_from(serve, "/");

  CAPTURE(cut);
  REQUIRE(got.status == http::status::ok);
  REQUIRE(got.body == "ping");
  REQUIRE(got.error == http::error::bad_message);
}

TEST_CASE("http::client: a body taken out of its response keeps what reads it", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto const body = (co_await client.stream(make_request(verb::get, "/hello"))).body;
    std::string seen;

    while (auto const piece = co_await body.read()) {
      seen += *piece;
    }

    co_return seen;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello");
}

TEST_CASE("http::client: what stream() refuses before it runs, as send() does", "[client]") {
  async::run_loop loop;
  memory_server server{loop.get_executor(), answering({}), 0, {}, {}};
  http::client const client{memory_connector{server}, "example.com", 80};

  REQUIRE_THROWS_AS(client.stream(make_request(verb::get, "http://example.com/")), std::invalid_argument);
}

TEST_CASE("http::client: to stream(), a redirect is a result, whatever max_redirects says", "[client]") {
  http::client_options options;

  options.max_redirects = 3;

  auto const got =
      stream_from(answering({"HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n"}), "/", options);

  REQUIRE(got.status == http::status::found);
  REQUIRE(got.connections == 1);
}

TEST_CASE("http::client: a stream's connection is kept only if it can carry another request", "[client]") {
  auto const reply = GENERATE(
      // Says it closes.
      std::string{"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi"},
      // Bytes past the response, which would read as the next one.
      std::string{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiextra"});

  async::run_loop loop;
  memory_server server{loop.get_executor(), answering({reply}, true), 0, {}, {}};

  auto const reading = [&]() -> task<int> {
    http::client const client{memory_connector{server}, "example.com", 80};

    for (auto attempt = 0; attempt < 2; ++attempt) {
      auto response = co_await client.stream(make_request(verb::get, "/"));

      while (co_await response.body.read()) {
      }
    }

    co_return server.connections;
  };

  CAPTURE(reply);
  REQUIRE(quoll::test::run(loop, reading()) == 2);
}

TEST_CASE("http::client: the place of a stream goes with its last piece, not with a read after it", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};
    auto response = co_await client.stream(make_request(verb::get, "/hello"));
    auto const piece = co_await response.body.read();

    // At work on the last piece, with no other read: this waits for no place.
    auto const other = co_await client.send(make_request(verb::get, "/hello"));

    co_return std::string{*piece} + "|" + other.body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello|hello");
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: a response that came whole keeps nothing, read or not", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};

    // Dropped without reading, then held on to: neither holds up what follows, nor costs a connection.
    static_cast<void>(co_await client.stream(make_request(verb::get, "/hello")));

    auto const kept = co_await client.stream(make_request(verb::get, "/hello"));
    auto const other = co_await client.send(make_request(verb::get, "/hello"));
    auto const piece = co_await kept.body.read();

    co_return std::string{*piece} + "|" + other.body + (co_await kept.body.read() ? "|more" : "|end");
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello|hello|end");
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: what is at fault after a good piece frees its place at once", "[client]") {
  auto const in_one_read = GENERATE(false, true);
  auto connections = 0;

  auto const serve = [&](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    ++connections;

    auto const this_one = connections;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    std::string_view const head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string_view const rest = "4\r\nping\r\nZ\r\n";
    std::string_view const other = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

    if (this_one != 1) {
      (void)co_await io::write(stream, std::as_bytes(std::span{other}));
      co_return;
    }

    if (in_one_read) {
      auto const whole = std::string{head} + std::string{rest};

      (void)co_await io::write(stream, std::as_bytes(std::span{whole}));
      co_return;
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));

    for (auto turn = 0; turn < 16; ++turn) {
      co_await async::yield();
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{rest}));
  };

  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};
    auto response = co_await client.stream(make_request(verb::get, "/"));
    auto const piece = co_await response.body.read();
    auto seen = std::string{*piece};

    // Before reading on: this waits for no place held by a connection that can do nothing more.
    seen += "|" + (co_await client.send(make_request(verb::get, "/"))).body;

    try {
      static_cast<void>(co_await response.body.read());
      seen += "|no failure";
    } catch (std::system_error const &) {
      seen += "|failed";
    }

    co_return seen;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "ping|hi|failed");
}

TEST_CASE("http::client: stream() completes with the head, before its body has come", "[client]") {
  auto may_send_the_rest = false;

  auto const serve = [&](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    if (co_await http::read(stream, buffer, parser)) {
      co_return;
    }

    std::string_view const head = "HTTP/1.1 201 Made\r\nX-Head: yes\r\nContent-Length: 4\r\n\r\n";
    std::string_view const rest = "ping";

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));

    while (!may_send_the_rest) {
      co_await async::yield();
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{rest}));
  };

  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};
  http::streamed_response seen;

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};

    seen = co_await client.stream(make_request(verb::get, "/"));
    may_send_the_rest = true;

    std::string body;

    while (auto const piece = co_await seen.body.read()) {
      body += *piece;
    }

    co_return body;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "ping");
  REQUIRE(seen.status == http::status::created);
  REQUIRE(seen.reason == "Made");
  REQUIRE(seen.version == http::version::http_1_1);
  REQUIRE(seen.fields.find("X-Head") == "yes");
}

TEST_CASE("http::client: copies of a body read as one, and one moved from has nothing to read", "[client]") {
  auto const router = make_streaming_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto response = co_await client.stream(make_request(verb::get, "/hello"));
    auto const copy = response.body;
    auto const moved = std::move(response.body);
    std::string seen;

    if (co_await response.body.read()) { // NOLINT(bugprone-use-after-move)
      seen += "moved from|";
    }

    seen += *(co_await copy.read());
    seen += co_await moved.read() ? "|more" : "|end";
    co_return seen;
  };

  REQUIRE(quoll::test::run(loop, reading()) == "hello|end");
}

namespace {

// What closes reads as a reset, as on a socket its peer cut.
class resetting_stream {
public:
  explicit resetting_stream(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
    auto read = co_await inner_.read_some(buffer);

    if (read.error == io::error::end_of_stream) {
      read.error = std::make_error_code(std::errc::connection_reset);
    }

    co_return read;
  }

  // What a socket being torn down may say to a write, other than a broken pipe.
  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    auto wrote = co_await inner_.write_some(data);

    if (wrote.error == std::errc::broken_pipe) {
      wrote.error = std::make_error_code(std::errc::connection_aborted);
    }

    co_return wrote;
  }

private:
  io::memory_stream inner_;
};

class resetting_connector {
public:
  explicit resetting_connector(memory_server &server) noexcept : inner_(server) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    auto made = co_await inner_.connect(std::move(host), port);

    co_return io::any_stream{resetting_stream{std::move(*made.target<io::memory_stream>())}};
  }

private:
  memory_connector inner_;
};

// A connector with something to see its end by.
class watched_connector {
public:
  watched_connector(memory_server &server, std::shared_ptr<int> watched) noexcept
      : inner_(server), watched_(std::move(watched)) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return inner_.connect(std::move(host), port);
  }

private:
  memory_connector inner_;
  std::shared_ptr<int> watched_;
};

} // namespace

TEST_CASE("http::client: stream() tries again what closes during the body of a first interim response", "[client]") {
  auto connections = 0;

  auto const serve = [&](io::memory_stream stream) {
    ++connections;

    return connections == 1 ? answer(std::move(stream),
                                     {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi",
                                      "HTTP/1.1 150 Odd\r\nContent-Length: 4\r\n\r\n"},
                                     false)
                            : answer(std::move(stream), {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain"}, false);
  };

  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto const reading = [&]() -> task<std::string> {
    http::client const client{resetting_connector{server}, "example.com", 80};

    static_cast<void>(co_await client.send(make_request(verb::get, "/")));

    auto response = co_await client.stream(make_request(verb::get, "/"));
    auto const piece = co_await response.body.read();

    co_return std::string{*piece};
  };

  REQUIRE(quoll::test::run(loop, reading()) == "again");
  REQUIRE(connections == 2);
}

TEST_CASE("http::client: a response kept past its end does not keep its client", "[client]") {
  auto const router = make_streaming_router();
  auto const cut_short = GENERATE(false, true);
  auto const serve = cut_short ? answering({"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nping"}) : serving(router);
  async::run_loop loop;
  memory_server server{loop.get_executor(), serve, 0, {}, {}};

  auto watched = std::make_shared<int>(0);
  std::weak_ptr<int> const seen = watched;

  auto const reading = [&]() -> task<bool> {
    std::optional<http::streamed_response> kept;

    {
      http::client const client{watched_connector{server, std::move(watched)}, "example.com", 80};

      kept = co_await client.stream(make_request(verb::get, "/pieces"));

      try {
        while (co_await kept->body.read()) {
        }
      } catch (std::system_error const &) {
      }
    }

    co_return seen.expired();
  };

  REQUIRE(quoll::test::run(loop, reading()) == true);
}

TEST_CASE("http::client: stream() has no limit on a body by default", "[client]") {
  auto const got = stream_from(answering({"HTTP/1.1 200 OK\r\nContent-Length: 5368709120\r\n\r\nping"}), "/");

  REQUIRE(got.body == "ping");
  REQUIRE(got.error == http::error::partial_message);
}

TEST_CASE("http::client: stream() keeps and takes an idle connection as send() does", "[client]") {
  // Each connection answers twice, for one kept to be seen to carry the second request rather than close under it.
  auto const whole = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
  auto const twice = [&](auto serve, verb method, http::client_options options = {}, std::string field = {}) {
    async::run_loop loop;
    memory_server server{loop.get_executor(), serve, 0, {}, {}};

    auto const reading = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80, options};
      auto first = make_request(verb::get, "/");

      if (!field.empty()) {
        first.fields.add("Connection", field);
      }

      auto ended = co_await client.stream(std::move(first));

      while (co_await ended.body.read()) {
      }

      // The other end has had the time to go, if it goes.
      for (auto turn = 0; turn < 16; ++turn) {
        co_await async::yield();
      }

      auto response = co_await client.stream(make_request(method, "/"));
      auto const piece = co_await response.body.read();

      co_return std::string{*piece};
    };

    REQUIRE(quoll::test::run(loop, reading()) == "hi");
    return server.connections;
  };

  SECTION("a GET takes it") { REQUIRE(twice(answering({whole, whole}, true), verb::get) == 1); }

  SECTION("a POST takes it too, as any request whose body is whole") {
    REQUIRE(twice(answering({whole, whole}, true), verb::post) == 1);
  }

  SECTION("a POST whose connection the server closed before it was written goes again on a new one") {
    REQUIRE(twice(answering({whole}), verb::post) == 2);
  }

  SECTION("whatever the server sent before closing it") {
    REQUIRE(twice(answering_then_closing(whole, service_unavailable), verb::post) == 2);
    REQUIRE(twice(answering_then_closing(whole, closing_timeout), verb::post) == 2);
  }

  SECTION("a POST the server closed it under once it was written fails") {
    async::run_loop loop;
    memory_server server{loop.get_executor(), answering({whole}, true), 0, {}, {}};

    auto const posting = [&]() -> task<std::error_code> {
      http::client const client{memory_connector{server}, "example.com", 80};

      auto const first = co_await client.send(make_request(verb::get, "/"));

      REQUIRE(first.body == "hi");
      co_return co_await quoll::test::code_thrown_by(client.stream(make_request(verb::post, "/")));
    };

    REQUIRE(quoll::test::run(loop, posting()) == io::error::end_of_stream);
    REQUIRE(server.connections == 1);
  }

  SECTION("one the server closed is replaced") { REQUIRE(twice(answering({whole}), verb::get) == 2); }

  SECTION("none is kept when none may be") {
    REQUIRE(twice(answering({whole, whole}, true), verb::get, {.max_idle_connections = 0}) == 2);
  }

  SECTION("none is kept when the request closes it") {
    REQUIRE(twice(answering({whole, whole}, true), verb::get, {}, "close") == 2);
  }

  SECTION("none is kept when the response closes it") {
    auto const closing = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";

    REQUIRE(twice(answering({closing, closing}, true), verb::get) == 2);
  }
}

TEST_CASE("http::client: what stops stream() before the head", "[client][timer]") {
  virtual_time time;
  http::router router;

  router.get("/silent", [](http::request) -> task<http::response> {
    co_await async::sleep_for(1h);
    co_return http::response{};
  });

  auto const failure_of = [&](http::client_options options, auto wrap) -> task<std::error_code> {
    http::client const client{timed_connector{time, router}, "example.com", 80, options};

    try {
      static_cast<void>(co_await wrap(client.stream(make_request(verb::get, "/silent"))));
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  SECTION("its response timeout") {
    auto const untouched = [](auto started) { return started; };

    REQUIRE(quoll::test::run(time, failure_of({.response_timeout = 3s}, untouched)) ==
            std::make_error_code(std::errc::timed_out));
  }

  SECTION("a timeout of the caller's, that is a stop request") {
    auto const in_time = [](auto started) { return async::with_timeout(1s, std::move(started)); };

    REQUIRE(quoll::test::run(time, failure_of({.response_timeout = 0s}, in_time)) ==
            std::make_error_code(std::errc::timed_out));
  }
}

namespace {

// Writes until something stops it, saying what did.
http::body_writer writing_until_stopped(std::string &stopped_by) {
  return [&stopped_by](http::body_stream out) -> task<> {
    try {
      for (;;) {
        co_await out.write("x");
        co_await async::yield();
      }
    } catch (std::system_error const &thrown) {
      stopped_by = thrown.code() == std::errc::operation_canceled ? "a stop" : "a failed write";
      throw;
    }
  };
}

http::router make_upload_router() {
  http::router router;

  router.post("/echo", [](http::request message) {
    http::response response;

    response.fields.add("X-Framing", message.fields.contains("Transfer-Encoding") ? "chunks" : "length");
    response.fields.add("X-Trailer", message.trailers.find("X-Sum").value_or("none"));
    response.body = std::to_string(message.body.size());
    return response;
  });
  router.put("/echo", [](http::request message) {
    http::response response;

    response.body = std::to_string(message.body.size());
    return response;
  });
  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  return router;
}

// A server that answers from the head of a request, without reading the rest of it, and closes or not.
std::function<task<>(io::memory_stream)> answering_at_the_head(std::string reply, bool then_close,
                                                               std::string *received = nullptr) {
  return [reply = std::move(reply), then_close, received](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    if (co_await http::read_head(stream, buffer, parser)) {
      co_return;
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{reply}));

    if (then_close) {
      co_return;
    }

    // What is still coming, for as long as it comes.
    std::array<char, 4096> chunk{};

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        co_return;
      }

      if (received != nullptr) {
        received->append(chunk.data(), read.transferred);
      }
    }
  };
}

} // namespace

TEST_CASE("http::client: send() sends a body_writer's body in pieces", "[client]") {
  auto const router = make_upload_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  SECTION("in chunks, on a connection then kept") {
    auto runs = 0;

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      auto const answer = co_await client.send(upload_of(pieces_of(4096, 256, &runs)));
      auto const hello = co_await client.send(make_request(verb::get, "/hello"));

      co_return answer.body + " " + std::string{answer.fields.find("X-Framing").value_or("")} + " " + hello.body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "1048576 chunks hello");
    REQUIRE(runs == 1);
    REQUIRE(server.connections == 1);
  }

  SECTION("with its trailers") {
    auto request = upload_of(pieces_of(1, 3));

    request.trailers.add("X-Sum", "7");

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      auto const answer = co_await client.send(std::move(request));

      co_return std::string{answer.fields.find("X-Trailer").value_or("")};
    };

    REQUIRE(quoll::test::run(loop, sending()) == "7");
  }

  SECTION("of the length it says") {
    auto request = upload_of(pieces_of(2, 3));

    request.fields.set("Content-Length", "6");

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      auto const answer = co_await client.send(std::move(request));

      co_return answer.body + " " + std::string{answer.fields.find("X-Framing").value_or("")};
    };

    REQUIRE(quoll::test::run(loop, sending()) == "6 length");
  }

  SECTION("nothing at all") {
    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      co_return (co_await client.send(upload_of(pieces_of(0, 0)))).body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "0");
  }

  SECTION("the head on its own, if the writer asks") {
    auto const writer = [](http::body_stream out) -> task<> {
      co_await out.write("");
      co_await out.write("ab");
    };
    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      co_return (co_await client.send(upload_of(writer))).body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "2");
  }
}

TEST_CASE("http::client: what a body_writer gets wrong comes out of send()", "[client]") {
  auto const router = make_upload_router();
  auto connections_after = 0;
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const failing = [&](http::request request) -> task<std::string> {
    http::client_options options;

    // A place it did not give back would hold up what follows for ever.
    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};
    std::string seen;

    try {
      static_cast<void>(co_await client.send(std::move(request)));
      seen = "no failure";
    } catch (std::length_error const &) {
      seen = "length";
    } catch (std::runtime_error const &thrown) {
      seen = thrown.what();
    }

    // Its connection is gone, and no other is held up.
    seen += " " + (co_await client.send(make_request(verb::get, "/hello"))).body;
    connections_after = server.connections;
    co_return seen;
  };

  SECTION("more than its Content-Length") {
    auto request = upload_of(pieces_of(4, 2));

    request.fields.set("Content-Length", "6");
    auto const got = quoll::test::run(loop, failing(std::move(request)));

    REQUIRE(got == "length hello");
  }

  SECTION("less than its Content-Length") {
    auto request = upload_of(pieces_of(4, 1));

    request.fields.set("Content-Length", "6");
    auto const got = quoll::test::run(loop, failing(std::move(request)));

    REQUIRE(got == "length hello");
  }

  SECTION("what it throws") {
    auto const throwing = [](http::body_stream out) -> task<> {
      co_await out.write("ab");
      throw std::runtime_error{"from the writer"};
    };

    REQUIRE(quoll::test::run(loop, failing(upload_of(throwing))) == "from the writer hello");
  }

  SECTION("what it makes of a write past its length, which it then ignores") {
    auto request = upload_of([](http::body_stream out) -> task<> {
      co_await out.write("four");

      try {
        co_await out.write("more");
      } catch (std::length_error const &) {
      }
    });

    request.fields.set("Content-Length", "6");

    auto const swallowed = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};
      std::string seen;

      try {
        static_cast<void>(co_await client.send(std::move(request)));
        seen = "no failure";
      } catch (std::system_error const &thrown) {
        seen = thrown.code() == std::errc::message_size ? "its length" : "another failure";
      }

      // Nothing else went on that connection, and another carries this.
      seen += " " + (co_await client.send(make_request(verb::get, "/hello"))).body;
      connections_after = server.connections;
      co_return seen;
    };

    REQUIRE(quoll::test::run(loop, swallowed()) == "its length hello");
  }

  SECTION("what it throws before anything left") {
    auto const throwing = [](http::body_stream) -> task<> {
      throw std::runtime_error{"at once"};
      co_return;
    };

    REQUIRE(quoll::test::run(loop, failing(upload_of(throwing))) == "at once hello");
  }

  REQUIRE(connections_after == 2);
}

TEST_CASE("http::client: send() refuses a body_writer it could not send, before its task runs", "[client]") {
  async::run_loop loop;
  auto const router = make_upload_router();
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};
  http::client const client{memory_connector{server}, "example.com", 80};
  auto request = upload_of(pieces_of(1, 1));

  SECTION("a body as well") { request.body = "whole"; }

  SECTION("HTTP/1.0 without a length") { request.version = http::version::http_1_0; }

  SECTION("a TRACE") { request.method = verb::trace; }

  SECTION("a CONNECT") {
    request.method = verb::connect;
    request.target = "/";
  }

  REQUIRE_THROWS_AS(client.send(std::move(request)), std::invalid_argument);
  REQUIRE(server.connections == 0);
}

TEST_CASE("http::client: a body_writer runs once, on a connection of its own", "[client]") {
  auto const whole = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
  auto runs = 0;

  SECTION("no idle one, for a PUT either") {
    async::run_loop loop;
    memory_server server{loop.get_executor(), answering({whole, whole}, true), 0, {}, {}};

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      static_cast<void>(co_await client.send(make_request(verb::get, "/")));
      co_return (co_await client.send(upload_of(pieces_of(1, 1, &runs), verb::put, "/"))).body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "hi");
    REQUIRE(server.connections == 2);
  }

  SECTION("not sent again when its connection closes") {
    auto const got = [&] {
      async::run_loop loop;
      memory_server server{loop.get_executor(), answering({}), 0, {}, {}};

      auto const sending = [&]() -> task<std::error_code> {
        http::client const client{memory_connector{server}, "example.com", 80};

        try {
          static_cast<void>(co_await client.send(upload_of(pieces_of(1, 1, &runs), verb::put, "/")));
        } catch (std::system_error const &thrown) {
          co_return thrown.code();
        }

        co_return std::error_code{};
      };

      auto const error = quoll::test::run(loop, sending());

      REQUIRE(server.connections == 1);
      return error;
    }();

    REQUIRE(got == io::error::end_of_stream);
  }

  REQUIRE(runs == 1);
}

TEST_CASE("http::client: a 3xx to a body_writer is a result, whatever max_redirects says", "[client]") {
  async::run_loop loop;
  memory_server server{loop.get_executor(),
                       answering({"HTTP/1.1 307 Again\r\nLocation: /other\r\nContent-Length: 0\r\n\r\n"}),
                       0,
                       {},
                       {}};

  auto const sending = [&]() -> task<http::status> {
    http::client const client{memory_connector{server}, "example.com", 80, {.max_redirects = 2}};

    co_return (co_await client.send(upload_of(pieces_of(1, 1)))).status;
  };

  REQUIRE(quoll::test::run(loop, sending()) == http::status::temporary_redirect);
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: an answer before the end of a body_writer stops it", "[client]") {
  auto const refused = std::string{"HTTP/1.1 413 Too Big\r\nContent-Length: 2\r\n\r\nno"};
  std::string stopped_by;

  auto const after = [&](auto serve, auto call) {
    async::run_loop loop;
    memory_server server{loop.get_executor(), serve, 0, {}, {}};

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};

      co_return co_await call(client);
    };

    return quoll::test::run(loop, sending());
  };

  auto const by_send = [&](http::client const &client) -> task<std::string> {
    auto const answer = co_await client.send(upload_of(writing_until_stopped(stopped_by)));

    co_return std::to_string(static_cast<int>(answer.status)) + " " + answer.body;
  };

  SECTION("a server reading on: the answer stops it") {
    auto const got = after(answering_at_the_head(refused, false), by_send);

    REQUIRE(got == "413 no");
    REQUIRE(stopped_by == "a stop");
  }

  SECTION("a server that went: the write that fails stops it, and its answer stands") {
    auto const got = after(answering_at_the_head(refused, true), by_send);

    REQUIRE(got == "413 no");
    REQUIRE(stopped_by == "a failed write");
  }

  SECTION("a server that went: what the writer makes of it is the writer's") {
    auto const rethrowing_as = [](auto own) {
      return [own](http::body_stream out) -> task<> {
        try {
          for (;;) {
            co_await out.write("x");
            co_await async::yield();
          }
        } catch (std::system_error const &) {
          throw own;
        }
      };
    };

    auto const by_send_failing = [&](http::body_writer writer) {
      return [&, writer = std::move(writer)](http::client const &client) -> task<std::string> {
        try {
          co_return (co_await client.send(upload_of(writer))).body;
        } catch (std::exception const &thrown) {
          co_return thrown.what();
        }
      };
    };

    // Of no kind that a write throws, and of the very kind, with another code.
    REQUIRE(after(answering_at_the_head(refused, true),
                  by_send_failing(rethrowing_as(std::runtime_error{"from the writer"}))) == "from the writer");

    auto const own = std::system_error{std::make_error_code(std::errc::io_error), "from the writer"};

    REQUIRE(after(answering_at_the_head(refused, true), by_send_failing(rethrowing_as(own))) ==
            std::string{own.what()});
  }

  SECTION("a server that went: a writer making nothing of it ends well, and the answer stands") {
    auto const carrying_on = [](http::body_stream out) -> task<> {
      try {
        for (;;) {
          co_await out.write("x");
          co_await async::yield();
        }
      } catch (std::system_error const &) {
      }
    };

    auto const by_send_quietly = [&](http::client const &client) -> task<std::string> {
      co_return (co_await client.send(upload_of(carrying_on))).body;
    };

    REQUIRE(after(answering_at_the_head(refused, true), by_send_quietly) == "no");
  }

  SECTION("a writer that takes no notice, then ends well") {
    auto const carrying_on = [&stopped_by](http::body_stream out) -> task<> {
      for (auto turn = 0; turn < 64; ++turn) {
        try {
          co_await out.write("x");
        } catch (std::system_error const &) {
          stopped_by = "taken no notice of";
        }

        co_await async::yield();
      }
    };

    auto const by_send_too = [&](http::client const &client) -> task<std::string> {
      auto const body = (co_await client.send(upload_of(carrying_on))).body;

      // For the server to have read whatever left.
      for (auto turn = 0; turn < 32; ++turn) {
        co_await async::yield();
      }

      co_return body;
    };

    std::string received;
    auto const got = after(answering_at_the_head(refused, false, &received), by_send_too);

    REQUIRE(got == "no");
    REQUIRE(stopped_by == "taken no notice of");
    // A body that was cut must not read as whole.
    REQUIRE(received.find("0\r\n\r\n") == std::string::npos);
  }

  SECTION("through stream(), from a server that went") {
    auto const by_stream = [&](http::client const &client) -> task<std::string> {
      auto response = co_await client.stream(upload_of(writing_until_stopped(stopped_by)));
      std::string body;

      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }

      co_return std::to_string(static_cast<int>(response.status)) + " " + body;
    };

    REQUIRE(after(answering_at_the_head(refused, true), by_stream) == "413 no");
  }

  SECTION("through stream(), at the head") {
    auto const by_stream = [&](http::client const &client) -> task<std::string> {
      auto response = co_await client.stream(upload_of(writing_until_stopped(stopped_by)));
      std::string body;

      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }

      co_return std::to_string(static_cast<int>(response.status)) + " " + body;
    };

    auto const got = after(answering_at_the_head(refused, false), by_stream);

    REQUIRE(got == "413 no");
    REQUIRE(stopped_by == "a stop");
  }
}

TEST_CASE("http::client: what is cut short is not reused, and what left whole is", "[client]") {
  auto const whole = std::string{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"};
  std::string stopped_by;
  async::run_loop loop;

  SECTION("an answer before the end") {
    auto const through_stream = GENERATE(false, true);
    auto connections = 0;

    auto const at_the_head = answering_at_the_head(whole, false);
    auto const at_the_end = answering({whole});

    auto const serve = [&](io::memory_stream stream) {
      ++connections;

      return connections == 1 ? at_the_head(std::move(stream)) : at_the_end(std::move(stream));
    };

    memory_server server{loop.get_executor(), serve, 0, {}, {}};

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};
      std::string first;

      if (through_stream) {
        auto response = co_await client.stream(upload_of(writing_until_stopped(stopped_by)));

        while (auto const piece = co_await response.body.read()) {
          first += *piece;
        }
      } else {
        first = (co_await client.send(upload_of(writing_until_stopped(stopped_by)))).body;
      }

      co_return first + (co_await client.send(make_request(verb::get, "/"))).body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "hihi");
    REQUIRE(connections == 2);
  }

  SECTION("a stream that left whole keeps its connection, with its response read in pieces") {
    auto const router = make_upload_router();
    memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

    auto const sending = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};
      auto response = co_await client.stream(upload_of(pieces_of(4096, 16)));
      std::string body;

      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }

      co_return body + " " + (co_await client.send(make_request(verb::get, "/hello"))).body;
    };

    REQUIRE(quoll::test::run(loop, sending()) == "65536 hello");
    REQUIRE(server.connections == 1);
  }
}

TEST_CASE("http::client: the timeouts of a body_writer", "[client][timer]") {
  virtual_time time;
  http::router router;

  router.post("/echo", [](http::request message) {
    http::response response;

    response.body = std::to_string(message.body.size());
    return response;
  });
  router.post("/silent", [](http::request) -> task<http::response> {
    co_await async::sleep_for(1h);
    co_return http::response{};
  });

  auto const outcome_of = [&](http::request request, http::client_options options,
                              std::chrono::nanoseconds own_limit = {}) -> task<std::string> {
    http::client const client{timed_connector{time, router}, "example.com", 80, options};
    auto const started = time.now();

    try {
      auto sent = client.send(std::move(request));

      if (own_limit == std::chrono::nanoseconds{}) {
        co_return (co_await std::move(sent)).body;
      }

      co_return (co_await async::with_timeout(own_limit, std::move(sent))).body;
    } catch (std::system_error const &thrown) {
      auto const taken = std::chrono::duration_cast<std::chrono::seconds>(time.now() - started);

      co_return (thrown.code() == std::errc::timed_out ? "timed out at " : "another failure at ") +
          std::to_string(taken.count());
    }
  };

  SECTION("the writer takes its time: none of it counts") {
    auto const writer = [](http::body_stream out) -> task<> {
      for (auto index = 0; index < 3; ++index) {
        co_await async::sleep_for(5s);
        co_await out.write("part");
      }
    };

    REQUIRE(quoll::test::run(time, outcome_of(upload_of(writer), {.response_timeout = 3s})) == "12");
  }

  SECTION("the response, from the end of the body") {
    auto const writer = [](http::body_stream out) -> task<> {
      co_await async::sleep_for(5s);
      co_await out.write("part");
    };

    REQUIRE(quoll::test::run(time, outcome_of(upload_of(writer, verb::post, "/silent"), {.response_timeout = 3s})) ==
            "timed out at 8");
  }

  SECTION("through stream(), the head, from the end of the body") {
    auto const writer = [](http::body_stream out) -> task<> {
      co_await async::sleep_for(5s);
      co_await out.write("part");
    };

    auto const streaming = [&]() -> task<std::string> {
      http::client const client{timed_connector{time, router}, "example.com", 80, {.response_timeout = 3s}};
      auto const started = time.now();

      try {
        static_cast<void>(co_await client.stream(upload_of(writer, verb::post, "/silent")));
      } catch (std::system_error const &thrown) {
        auto const taken = std::chrono::duration_cast<std::chrono::seconds>(time.now() - started);

        co_return (thrown.code() == std::errc::timed_out ? "timed out at " : "another failure at ") +
            std::to_string(taken.count());
      }

      co_return "a head";
    };

    REQUIRE(quoll::test::run(time, streaming()) == "timed out at 8");
  }

  SECTION("a stop request reaches the writer") {
    std::string stopped_by;
    auto const writer = [&stopped_by](http::body_stream) -> task<> {
      try {
        co_await async::sleep_for(1h);
      } catch (std::system_error const &thrown) {
        stopped_by = thrown.code() == std::errc::operation_canceled ? "a stop" : "something else";
        throw;
      }
    };

    REQUIRE(quoll::test::run(time, outcome_of(upload_of(writer), {.response_timeout = 0s}, 2s)) == "timed out at 2");
    REQUIRE(stopped_by == "a stop");
  }
}

TEST_CASE("http::client: a server that went stands however its socket says so to a write", "[client]") {
  async::run_loop loop;
  std::string stopped_by;
  memory_server server{loop.get_executor(),
                       answering_at_the_head("HTTP/1.1 413 Too Big\r\nContent-Length: 2\r\n\r\nno", true),
                       0,
                       {},
                       {}};

  auto const sending = [&]() -> task<std::string> {
    http::client const client{resetting_connector{server}, "example.com", 80};

    co_return (co_await client.send(upload_of(writing_until_stopped(stopped_by)))).body;
  };

  REQUIRE(quoll::test::run(loop, sending()) == "no");
  REQUIRE(stopped_by == "a failed write");
}

TEST_CASE("http::client: an interim response does not stop a body_writer", "[client]") {
  http::router router;

  router.post("/count", [](http::body_reader body) -> task<http::response> {
    std::size_t size = 0;

    while (auto const piece = co_await body.read()) {
      size += piece->size();
    }

    http::response response;

    response.body = std::to_string(size);
    co_return response;
  });
  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });

  auto const through_stream = GENERATE(false, true);
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const sending = [&]() -> task<std::string> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto request = upload_of(pieces_of(4096, 16), verb::post, "/count");
    std::string got;

    request.fields.set("Expect", "100-continue");

    if (through_stream) {
      auto response = co_await client.stream(std::move(request));

      while (auto const piece = co_await response.body.read()) {
        got += *piece;
      }
    } else {
      got = (co_await client.send(std::move(request))).body;
    }

    co_return got + " " + (co_await client.send(make_request(verb::get, "/hello"))).body;
  };

  REQUIRE(quoll::test::run(loop, sending()) == "65536 hello");
  REQUIRE(server.connections == 1);
}

TEST_CASE("http::client: an upload holds its place in max_connections until it has ended", "[client]") {
  auto const router = make_upload_router();
  auto const through_stream = GENERATE(false, true);
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};
  std::vector<std::string> order;

  auto const sending = [&]() -> task<int> {
    http::client_options options;

    options.max_connections = 1;

    http::client const client{memory_connector{server}, "example.com", 80, options};

    auto const uploading = [&]() -> task<int> {
      auto const writer = [&order](http::body_stream out) -> task<> {
        order.emplace_back("upload begins");

        for (auto turn = 0; turn < 8; ++turn) {
          co_await out.write("x");
          co_await async::yield();
        }

        order.emplace_back("upload ends");
      };

      if (through_stream) {
        auto response = co_await client.stream(upload_of(writer));

        while (co_await response.body.read()) {
        }
      } else {
        static_cast<void>(co_await client.send(upload_of(writer)));
      }

      co_return 1;
    };

    auto const asking = [&]() -> task<int> {
      static_cast<void>(co_await client.send(make_request(verb::get, "/hello")));
      order.emplace_back("the other answered");
      co_return 2;
    };

    auto [one, two] = co_await async::when_all(uploading(), asking());

    co_return one + two;
  };

  REQUIRE(quoll::test::run(loop, sending()) == 3);
  REQUIRE(order == std::vector<std::string>{"upload begins", "upload ends", "the other answered"});
}

namespace {

// A stream whose writes fail with `failure`, and whose reads give `reply`, then its end.
class failing_writes {
public:
  failing_writes(std::error_code failure, std::string reply) : failure_(failure), reply_(std::move(reply)) {}

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
    if (reply_.empty()) {
      co_return io::io_result{io::make_error_code(io::error::end_of_stream), 0};
    }

    auto const size = std::min(buffer.size(), reply_.size());

    std::memcpy(buffer.data(), reply_.data(), size);
    reply_.erase(0, size);
    co_return io::io_result{{}, size};
  }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) { co_return io::io_result{failure_, 0}; }

private:
  std::error_code failure_;
  std::string reply_;
};

class failing_writes_connector {
public:
  failing_writes_connector(std::error_code failure, std::string reply) : failure_(failure), reply_(std::move(reply)) {}

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    co_return io::any_stream{failing_writes{failure_, reply_}};
  }

private:
  std::error_code failure_;
  std::string reply_;
};

// Nothing will come, its writes fail, and the connection stays open: a read beside it would wait for ever.
class never_answers {
public:
  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) {
    co_await async::detail::slots{0}.acquire();
    co_return io::io_result{};
  }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    co_return io::io_result{std::make_error_code(std::errc::io_error), 0};
  }
};

struct never_answers_connector {
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    co_return io::any_stream{never_answers{}};
  }
};

// Closed by the server while a write is on its way, which waits until it is stopped.
class closing_during_a_write {
public:
  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte>) {
    co_await async::yield();
    co_return io::io_result{io::make_error_code(io::error::end_of_stream), 0};
  }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    co_await async::detail::slots{0}.acquire();
    co_return io::io_result{};
  }
};

struct closing_during_a_write_connector {
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    co_return io::any_stream{closing_during_a_write{}};
  }
};

} // namespace

TEST_CASE("http::client: a server that answers and goes is heard, though the request could not leave", "[client]") {
  async::run_loop loop;
  auto const gone = GENERATE(std::errc::broken_pipe, std::errc::connection_reset, std::errc::connection_aborted,
                             std::errc::not_connected, std::errc::network_reset);
  std::string const refusal = "HTTP/1.1 413 Too Large\r\nContent-Length: 2\r\n\r\nno";
  http::client const client{failing_writes_connector{std::make_error_code(gone), refusal}, "example.com", 80};

  auto const sending = [&]() -> task<int> {
    auto const whole = co_await client.send(make_request(verb::post, "/", "body"));
    auto stream = co_await client.stream(make_request(verb::post, "/", "body"));
    auto const piece = co_await stream.body.read();

    co_return static_cast<int>(whole.status) * 1000 + static_cast<int>(stream.status) + (piece == "no" ? 1000000 : 0);
  };

  REQUIRE(quoll::test::run(loop, sending()) == 1'000'000 + 413 * 1000 + 413);
}

TEST_CASE("http::client: a server that goes with no answer says the request could not leave", "[client]") {
  async::run_loop loop;
  http::client const client{failing_writes_connector{std::make_error_code(std::errc::broken_pipe), ""}, "example.com",
                            80};

  auto const code_of = [](auto work) -> task<std::error_code> {
    try {
      co_await std::move(work);
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  auto const whole = [&]() -> task<> { (void)co_await client.send(make_request(verb::post, "/", "body")); };
  auto const streamed = [&]() -> task<> { (void)co_await client.stream(make_request(verb::post, "/", "body")); };
  auto const in_pieces = [&]() -> task<> {
    auto request = make_request(verb::post, "/");

    request.body_writer = [](http::body_stream out) -> task<> { co_await out.write("body"); };
    (void)co_await client.send(std::move(request));
  };

  REQUIRE(quoll::test::run(loop, code_of(whole())) == std::errc::broken_pipe);
  REQUIRE(quoll::test::run(loop, code_of(streamed())) == std::errc::broken_pipe);
  REQUIRE(quoll::test::run(loop, code_of(in_pieces())) == std::errc::broken_pipe);
}

TEST_CASE("http::client: what came of a server gone is what it says, a bad message rather than a write", "[client]") {
  async::run_loop loop;
  http::client const client{failing_writes_connector{std::make_error_code(std::errc::broken_pipe), "not http\r\n\r\n"},
                            "example.com", 80};

  auto const code_of = [&](bool streamed) -> task<std::error_code> {
    try {
      if (streamed) {
        (void)co_await client.stream(make_request(verb::post, "/", "body"));
      } else {
        (void)co_await client.send(make_request(verb::post, "/", "body"));
      }
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(quoll::test::run(loop, code_of(false)) == http::error::bad_message);
  REQUIRE(quoll::test::run(loop, code_of(true)) == http::error::bad_message);
}

TEST_CASE("http::client: a server closing during a write is what the caller hears, no cancellation", "[client]") {
  async::run_loop loop;
  http::client_options options;

  options.response_timeout = {};

  http::client const client{closing_during_a_write_connector{}, "example.com", 80, options};
  auto const streamed = GENERATE(false, true);
  auto request = make_request(verb::post, "/");

  request.body_writer = [](http::body_stream out) -> task<> { co_await out.write("body"); };

  auto const sending = [&]() -> task<std::error_code> {
    try {
      if (streamed) {
        (void)co_await client.stream(std::move(request));
      } else {
        (void)co_await client.send(std::move(request));
      }
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(quoll::test::run(loop, sending()) == io::error::end_of_stream);
}

namespace {

// How much of its second request a checked_then_cut connection takes.
enum class taken : std::uint8_t { none, half, whole };

// Answers its first request, and is found open by a check after it; then takes some of the next request, as a server
// closing it as the request comes, and answers `last`, or nothing if empty. Taking it whole, it answers what comes
// next late, as a server answering that request after a 408. As a socket, it reads what it has whole.
class checked_then_cut {
public:
  checked_then_cut(taken second, std::string_view last) noexcept : second_(second), last_(last) {}

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
    std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

    if (replies_ > 0) {
      --replies_;
    } else if (writes_ > 1 && !last_.empty()) {
      reply = std::exchange(last_, std::string_view{});
    } else if (late_ > 0) {
      --late_;
      reply = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlate";
    } else {
      co_return io::io_result{io::make_error_code(io::error::end_of_stream), 0};
    }

    auto const size = std::min(reply.size(), buffer.size());

    std::memcpy(buffer.data(), reply.data(), size);
    co_return io::io_result{{}, size};
  }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> bytes) {
    auto const write = writes_++;

    if (write == 0) {
      ++replies_;
      co_return io::io_result{{}, bytes.size()};
    }

    if (write >= 1 && second_ == taken::whole) {
      late_ += write > 1 ? 1 : 0;
      co_return io::io_result{{}, bytes.size()};
    }

    if (write == 1 && second_ == taken::half) {
      co_return io::io_result{{}, bytes.size() / 2};
    }

    co_return io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
  }

  [[nodiscard]] io::read_readiness poll_read() noexcept { return io::read_readiness::would_wait; }

private:
  taken second_;
  std::string_view last_;
  int replies_ = 0;
  int late_ = 0;
  int writes_ = 0;
};

static_assert(io::PollableStream<checked_then_cut>);

struct checked_then_cut_connector {
  std::shared_ptr<int> count;
  taken second;
  std::string_view last;

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    ++*count;
    co_return io::any_stream{checked_then_cut{second, last}};
  }
};

struct post_outcome {
  std::error_code error;
  http::status status{};
  // Those opened for the GET and the POST.
  int connections = 0;
  // What a GET after them got.
  std::string next;
};

// A GET, then a POST on the connection it kept, which a check finds open, then cut as `second` and `last` say, then a
// GET.
post_outcome post_on_checked(taken second, std::string_view last, bool streamed) {
  async::run_loop loop;
  auto connections = std::make_shared<int>(0);
  http::client const client{checked_then_cut_connector{connections, second, last}, "example.com", 80};
  post_outcome outcome;

  auto const posting = [&]() -> task<bool> {
    auto const first = co_await client.send(make_request(verb::get, "/"));

    REQUIRE(first.body == "ok");

    try {
      if (streamed) {
        auto response = co_await client.stream(make_request(verb::post, "/", "ping"));

        while (co_await response.body.read()) {
        }

        outcome.status = response.status;
      } else {
        outcome.status = (co_await client.send(make_request(verb::post, "/", "ping"))).status;
      }
    } catch (std::system_error const &thrown) {
      outcome.error = thrown.code();
    }

    outcome.connections = *connections;
    outcome.next = (co_await client.send(make_request(verb::get, "/"))).body;
    co_return true;
  };

  REQUIRE(quoll::test::run(loop, posting()).value_or(false));
  return outcome;
}

} // namespace

TEST_CASE("http::client: a POST on a connection the server closes as it goes is sent again only if none of it left",
          "[client]") {
  auto const streamed = GENERATE(false, true);
  auto const closing_timeout = "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

  INFO("streamed " << streamed);

  // The server read none of it: it goes again on a new connection.
  auto const unsent = post_on_checked(taken::none, "", streamed);

  REQUIRE_FALSE(unsent.error);
  REQUIRE(unsent.status == http::status::ok);
  REQUIRE(unsent.connections == 2);

  // The server may have read what was written: the stream's failure is the request's, or a 408 its response, as the
  // server may send even one closing the connection once it has done the work.
  auto const half = post_on_checked(taken::half, "", streamed);

  REQUIRE(half.error == std::errc::broken_pipe);
  REQUIRE(half.connections == 1);

  auto const half_timed_out = post_on_checked(taken::half, closing_timeout, streamed);

  REQUIRE_FALSE(half_timed_out.error);
  REQUIRE(half_timed_out.status == http::status::request_timeout);
  REQUIRE(half_timed_out.connections == 1);

  auto const whole = post_on_checked(taken::whole, "", streamed);

  REQUIRE(whole.error == io::error::end_of_stream);
  REQUIRE(whole.connections == 1);

  auto const whole_timed_out = post_on_checked(taken::whole, closing_timeout, streamed);

  REQUIRE_FALSE(whole_timed_out.error);
  REQUIRE(whole_timed_out.status == http::status::request_timeout);

  // Nor is a connection kept that answered a 408 even without closing: the server may answer the POST on it after.
  auto const timed_out_open =
      post_on_checked(taken::whole, "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n\r\n", streamed);

  REQUIRE_FALSE(timed_out_open.error);
  REQUIRE(timed_out_open.status == http::status::request_timeout);
  REQUIRE(timed_out_open.connections == 1);
  REQUIRE(timed_out_open.next == "ok");
}

TEST_CASE("http::client: a POST opens its own connection when its stream cannot be checked", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  memory_server server{
      loop.get_executor(),
      [&](io::memory_stream stream) { return quoll::http::server{router}.serve_connection(std::move(stream)); },
      0,
      {},
      {}};

  // resetting_stream, over a memory stream, is no PollableStream: a GET still takes its connection.
  auto const sending = [&]() -> task<std::string> {
    http::client const client{resetting_connector{server}, "example.com", 80};
    auto const first = co_await client.send(make_request(verb::get, "/hello"));
    auto const second = co_await client.send(make_request(verb::post, "/echo", "ping"));
    auto const third = co_await client.send(make_request(verb::get, "/hello"));

    co_return first.body + second.body + third.body;
  };

  REQUIRE(quoll::test::run(loop, sending()).has_value());
  REQUIRE(server.connections == 2);
}

TEST_CASE("http::client: a connection whose request could not leave is not kept", "[client]") {
  async::run_loop loop;
  auto connections = std::make_shared<int>(0);

  // Its first write fails, and it answers it all the same: kept, it would serve the next request.
  class failing_once {
  public:
    [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
      if (replies_ == 0) {
        co_return io::io_result{io::make_error_code(io::error::end_of_stream), 0};
      }

      std::string_view const reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

      --replies_;
      std::memcpy(buffer.data(), reply.data(), reply.size());
      co_return io::io_result{{}, reply.size()};
    }

    [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> bytes) {
      if (std::exchange(first_write_, false)) {
        co_return io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
      }

      ++replies_;
      co_return io::io_result{{}, bytes.size()};
    }

  private:
    int replies_ = 1;
    bool first_write_ = true;
  };

  struct counting {
    std::shared_ptr<int> count;

    [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
      ++*count;
      co_return io::any_stream{failing_once{}};
    }
  };

  http::client const client{counting{connections}, "example.com", 80};
  auto const streamed = GENERATE(false, true);

  auto const twice = [&]() -> task<int> {
    for (auto request = 0; request < 2; ++request) {
      if (streamed) {
        auto response = co_await client.stream(make_request(verb::get, "/"));

        while (co_await response.body.read()) {
        }
      } else {
        (void)co_await client.send(make_request(verb::get, "/"));
      }
    }

    co_return *connections;
  };

  REQUIRE(quoll::test::run(loop, twice()) == 2);
}

TEST_CASE("http::client: a body that could not leave says so once the wait for an answer is over", "[client][timer]") {
  virtual_time time;
  http::client_options options;

  options.response_timeout = 3s;

  http::client const client{never_answers_connector{}, "example.com", 80, options};
  auto const streamed = GENERATE(false, true);
  auto request = make_request(verb::post, "/");

  request.body_writer = [](http::body_stream out) -> task<> { co_await out.write("body"); };

  auto const sending = [&]() -> task<std::error_code> {
    try {
      if (streamed) {
        (void)co_await client.stream(std::move(request));
      } else {
        (void)co_await client.send(std::move(request));
      }
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(quoll::test::run(time, sending()) == std::errc::io_error);
  REQUIRE(time.now() == 3s);
}

TEST_CASE("http::client: a write that fails with the connection still open ends the request, with no timer",
          "[client]") {
  async::run_loop loop;
  http::client_options options;

  options.response_timeout = {};

  http::client const client{never_answers_connector{}, "example.com", 80, options};
  auto request = make_request(verb::post, "/");

  request.body_writer = [](http::body_stream out) -> task<> { co_await out.write("body"); };

  auto const sending = [&]() -> task<std::error_code> {
    try {
      (void)co_await client.send(std::move(request));
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(quoll::test::run(loop, sending()) == std::errc::io_error);
}

TEST_CASE("http::client: a kept connection torn down goes again, however the socket says so", "[client]") {
  async::run_loop loop;
  auto served = 0;
  auto const router = make_router();
  memory_server server{loop.get_executor(),
                       [&](io::memory_stream stream) {
                         return ++served == 1 ? answer(std::move(stream),
                                                       {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, false)
                                              : quoll::http::server{router}.serve_connection(std::move(stream));
                       },
                       0,
                       {},
                       {}};
  auto const sending = [&]() -> task<std::string> {
    http::client const client{resetting_connector{server}, "example.com", 80};

    (void)co_await client.send(make_request(verb::get, "/"));

    // For the server to have closed it, so that the write fails rather than the read.
    for (auto turn = 0; turn < 32; ++turn) {
      co_await async::yield();
    }

    co_return (co_await client.send(make_request(verb::get, "/hello"))).body;
  };

  REQUIRE(quoll::test::run(loop, sending()) == "hello");
  REQUIRE(served == 2);
}

TEST_CASE("http::client: a stale connection leaves those given back since it was taken", "[client]") {
  async::run_loop loop;
  auto served = 0;
  auto const router = make_router();

  // Answers once, reads a second request, and only then closes, some turns later.
  auto const going_late = [](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser first;

    (void)co_await http::read(stream, buffer, first);

    std::string const reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

    (void)co_await io::write(stream, std::as_bytes(std::span{reply}));

    http::request_parser second;

    (void)co_await http::read(stream, buffer, second);

    for (auto turn = 0; turn < 256; ++turn) {
      co_await async::yield();
    }
  };

  memory_server server{loop.get_executor(),
                       [&](io::memory_stream stream) {
                         return ++served == 1 ? going_late(std::move(stream))
                                              : quoll::http::server{router}.serve_connection(std::move(stream));
                       },
                       0,
                       {},
                       {}};
  auto const streamed = GENERATE(false, true);
  auto const fetch = [streamed](http::client const &client, std::string target) -> task<int> {
    if (streamed) {
      auto response = co_await client.stream(make_request(verb::get, target));

      while (co_await response.body.read()) {
      }
    } else {
      (void)co_await client.send(make_request(verb::get, target));
    }

    co_return 0;
  };
  auto const sending = [&]() -> task<int> {
    http::client const client{memory_connector{server}, "example.com", 80};

    (void)co_await fetch(client, "/");
    // One finds its connection closed long after the other, on a new one, has given it back.
    auto [late, quick] = co_await async::when_all(fetch(client, "/hello"), fetch(client, "/hello"));

    static_cast<void>(late);
    static_cast<void>(quick);

    // Two are kept: the one given back, and the late one's new one.
    auto [third, fourth] = co_await async::when_all(fetch(client, "/hello"), fetch(client, "/hello"));

    static_cast<void>(third);
    static_cast<void>(fourth);
    co_return server.connections;
  };

  REQUIRE(quoll::test::run(loop, sending()) == 3);
}

TEST_CASE("http::client: a request stopped already leaves a kept connection alone", "[client]") {
  async::run_loop loop;
  auto const router = make_router();
  memory_server server{
      loop.get_executor(),
      [&router](io::memory_stream stream) { return quoll::http::server{router}.serve_connection(std::move(stream)); },
      0,
      {},
      {}};
  std::stop_source stopped;

  stopped.request_stop();

  auto const stopped_with = [&](auto request) -> task<std::error_code> {
    try {
      co_await async::detail::stop_scope<void>{std::move(request), stopped.get_token()};
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  auto const sending = [&]() -> task<int> {
    http::client const client{memory_connector{server}, "example.com", 80};

    (void)co_await client.send(make_request(verb::get, "/hello"));

    auto const whole = [&](http::request request) -> task<> { (void)co_await client.send(std::move(request)); };
    auto const streaming = [&](http::request request) -> task<> { (void)co_await client.stream(std::move(request)); };
    auto const get_whole = co_await stopped_with(whole(make_request(verb::get, "/hello")));
    auto const get_streaming = co_await stopped_with(streaming(make_request(verb::get, "/hello")));

    REQUIRE(get_whole == std::errc::operation_canceled);
    REQUIRE(get_streaming == std::errc::operation_canceled);

    (void)co_await client.send(make_request(verb::get, "/hello"));
    co_return server.connections;
  };

  REQUIRE(quoll::test::run(loop, sending()) == 1);

  // Nor does a request that would have opened one, whatever it is.
  auto const opening = [&]() -> task<int> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto const whole = [&](http::request request) -> task<> { (void)co_await client.send(std::move(request)); };
    auto const streaming = [&](http::request request) -> task<> { (void)co_await client.stream(std::move(request)); };
    auto const post_whole = co_await stopped_with(whole(make_request(verb::post, "/hello", "body")));
    auto const post_streaming = co_await stopped_with(streaming(make_request(verb::post, "/hello", "body")));

    REQUIRE(post_whole == std::errc::operation_canceled);
    REQUIRE(post_streaming == std::errc::operation_canceled);
    co_return server.connections;
  };

  REQUIRE(quoll::test::run(loop, opening()) == 1);
}

TEST_CASE("http::client: a URL or a host that would not read as itself is refused", "[client]") {
  for (std::string_view const url :
       {"http://example.com/%2F%2Fother.example", "http://example.com/%2e//other.example", "http://example.com/%09/x",
        "http://example.com/%09", "http://example.com/%0A", "http://example.com/%2e%09%2e/admin",
        "http://example.com/..%5Cadmin", "http://example.com/a/%09/b", "http://example.com/a/%2F/b",
        "http://example.com/a/%5C/b", "http://example.com/a/%09", "http://example.com/a%00b", "http://example.com:0/",
        "http://../x", "http://./", "http://.example.com/", "http://a..b/"}) {
    REQUIRE_THROWS_AS(http::client{std::string{url}}, std::invalid_argument);
  }

  REQUIRE_NOTHROW(http::client{"http://example.com./"});
  REQUIRE_NOTHROW(http::client{"http://example.com/"});
  REQUIRE_NOTHROW(http::client{"http://example.com/api/v1/"});
  REQUIRE_NOTHROW(http::client{"http://example.com/a%20b"});

  async::run_loop loop;
  memory_server server{loop.get_executor(), answering({}), 0, {}, {}};

  REQUIRE_THROWS_AS((http::client{memory_connector{server}, "a..b", 80}), std::invalid_argument);
  // A connector may make nothing of a port: it is the connector's to read.
  REQUIRE_NOTHROW(http::client{memory_connector{server}, "example.com", 0});
  REQUIRE_THROWS_AS((http::client{memory_connector{server}, "example.com", 65536}), std::invalid_argument);
}

namespace {

// Events written whole, a stream a server keeps open a while, and an event too large.
http::router make_event_router() {
  http::router router;

  router.get("/events", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.open();
    co_await events.retry(2s);
    co_await events.send({.type = "add", .data = "first\nline", .id = "1"});
    co_await events.comment();
    co_await events.send({.data = "second"});
  });
  router.get("/later", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.open();

    // Long enough for the reader to wait, and be moved meanwhile.
    for (int turn = 0; turn < 16; ++turn) {
      co_await async::yield();
    }

    co_await events.send({.data = "later"});
  });
  router.get("/large", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.send({.data = std::string(100, 'x')});
  });
  return router;
}

struct read_events {
  std::string content_type;
  std::vector<http::server_sent_event> events;
  std::string last_event_id;
  std::optional<std::chrono::milliseconds> retry;
  std::error_code error;
};

task<read_events> events_from(memory_server &server, std::string target, std::size_t max_event_size = 1024) {
  http::client const client{memory_connector{server}, "example.com", 80};
  read_events out;

  try {
    auto response = co_await client.stream(make_request(verb::get, std::move(target)));

    out.content_type = response.fields.find("Content-Type").value_or("");

    http::event_reader events{response.body, max_event_size};

    while (auto event = co_await events.next()) {
      out.events.push_back(std::move(*event));
    }

    out.last_event_id = events.last_event_id();
    out.retry = events.retry();
  } catch (std::system_error const &failure) {
    out.error = failure.code();
  }

  co_return out;
}

} // namespace

TEST_CASE("event_reader: the events an event_writer sends, as they come", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};
  auto const got = quoll::test::run(loop, events_from(server, "/events"));

  REQUIRE(got);
  REQUIRE(got->content_type == "text/event-stream");
  REQUIRE(got->events == std::vector<http::server_sent_event>{{.type = "add", .data = "first\nline", .id = "1"},
                                                              {.data = "second", .id = "1"}});
  REQUIRE(got->last_event_id == "1");
  REQUIRE(got->retry == 2s);
  REQUIRE_FALSE(got->error);
}

TEST_CASE("event_reader: an event larger than it holds fails the reading", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};
  auto const got = quoll::test::run(loop, events_from(server, "/large", 64));

  REQUIRE(got);
  REQUIRE(got->events.empty());
  REQUIRE(got->error == http::error::event_too_large);
}

TEST_CASE("event_reader: moved, or gone, while a next() waits, the read goes on", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<std::optional<http::server_sent_event>> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto response = co_await client.stream(make_request(verb::get, "/later"));
    std::optional<http::event_reader> events{std::in_place, std::move(response.body)};
    auto pending = events->next();

    // The read waits for the server while the reader is moved, then goes, the only one: the task alone holds what it
    // reads.
    auto const move_away = [](std::optional<http::event_reader> &from) -> task<> {
      { auto const gone = std::move(*from); }

      from.reset();
      co_return;
    };

    auto [event, nothing] = co_await async::when_all(std::move(pending), move_away(events));

    static_cast<void>(nothing);
    co_return event;
  };

  auto const got = quoll::test::run(loop, reading());

  REQUIRE(got);
  REQUIRE(*got == http::server_sent_event{.data = "later"});
}

TEST_CASE("event_reader: a second next() while one waits is refused", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<bool> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto response = co_await client.stream(make_request(verb::get, "/later"));
    http::event_reader events{response.body};

    auto const second = [](http::event_reader &reader) -> task<bool> {
      try {
        static_cast<void>(co_await reader.next());
      } catch (std::logic_error const &) {
        co_return true;
      }

      co_return false;
    };

    auto [first, refused] = co_await async::when_all(events.next(), second(events));

    co_return first == http::server_sent_event{.data = "later"} && refused;
  };

  REQUIRE(quoll::test::run(loop, reading()) == true);
}

TEST_CASE("event_reader: after a failure, next() fails the same, not as one pending", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  SECTION("an event too large") {
    auto const reading = [&]() -> task<int> {
      http::client const client{memory_connector{server}, "example.com", 80};
      auto response = co_await client.stream(make_request(verb::get, "/large"));
      http::event_reader events{response.body, 64};
      int too_large = 0;

      for (int attempt = 0; attempt < 2; ++attempt) {
        try {
          static_cast<void>(co_await events.next());
        } catch (std::system_error const &failure) {
          too_large += failure.code() == http::error::event_too_large ? 1 : 0;
        }
      }

      co_return too_large;
    };

    REQUIRE(quoll::test::run(loop, reading()) == 2);
  }

  SECTION("a stop request while it waits") {
    auto const reading = [&]() -> task<std::string> {
      http::client const client{memory_connector{server}, "example.com", 80};
      auto response = co_await client.stream(make_request(verb::get, "/later"));
      http::event_reader events{response.body};
      std::stop_source stop;
      std::exception_ptr first;
      bool finished = false;

      co_spawn(co_await async::current_executor(), events.next(),
               quoll::test::overloaded{[&finished](std::optional<http::server_sent_event>) { finished = true; },
                                       [&first, &finished](std::exception_ptr error) {
                                         first = std::move(error);
                                         finished = true;
                                       }},
               stop.get_token());
      co_await async::yield();
      stop.request_stop();

      while (!finished) {
        co_await async::yield();
      }

      if (!first) {
        co_return "not stopped";
      }

      // Stopped while it waited on the body, not failed some other way.
      try {
        std::rethrow_exception(first);
      } catch (std::system_error const &failure) {
        if (failure.code() != std::errc::operation_canceled) {
          co_return "failed otherwise";
        }
      }

      // The flag is down again: the second fails as the reading does, not as one pending.
      try {
        static_cast<void>(co_await events.next());
      } catch (std::logic_error const &) {
        co_return "pending";
      } catch (std::system_error const &) {
        co_return "ended";
      }

      co_return "read";
    };

    REQUIRE(quoll::test::run(loop, reading()) == "ended");
  }
}

TEST_CASE("event_reader: one moved from refuses next(), and has no id nor retry", "[client][sse]") {
  auto const router = make_event_router();
  async::run_loop loop;
  memory_server server{loop.get_executor(), serving(router), 0, {}, {}};

  auto const reading = [&]() -> task<bool> {
    http::client const client{memory_connector{server}, "example.com", 80};
    auto response = co_await client.stream(make_request(verb::get, "/events"));
    http::event_reader events{response.body};
    auto const first = co_await events.next();
    auto moved = std::move(events);

    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what is tested.
    bool refused = false;

    try {
      static_cast<void>(events.next());
    } catch (std::logic_error const &) {
      refused = true;
    }

    bool const empty = refused && events.last_event_id().empty() && !events.retry();
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

    bool const kept = moved.last_event_id() == "1" && moved.retry() == 2s;

    co_return first.has_value() && empty &&kept;
  };

  REQUIRE(quoll::test::run(loop, reading()) == true);
}
