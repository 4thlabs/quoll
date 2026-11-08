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

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client/client.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/connect_tls.hpp>
#include <quoll/io/listen_tls.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/run_task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using quoll::async::task;
using quoll::test::free_port;
using quoll::test::run_task;

http::request make_request(http::verb method, std::string target, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  request.body = std::move(body);

  if (method == http::verb::post) {
    request.fields.add("Content-Length", std::to_string(request.body.size()));
  }

  return request;
}

} // namespace

#if defined(QUOLL_ASIO_TLS)

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <quoll/asio/executor.hpp>
#include <quoll/asio/tls_acceptor.hpp>
#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/timeout_children.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connect.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/io/write.hpp>
#include <quoll/test/overloaded.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>

namespace {

using namespace std::chrono_literals;

using quoll::async::when_all;
using quoll::async::detail::stop_on_exit;
using quoll::async::detail::stop_scope;

std::string tls_file(std::string const &name) { return std::string{QUOLL_TEST_TLS_DIR} + "/" + name; }

// Named localhost and 127.0.0.1.
io::tls_server_options server_certificate() {
  return {.certificate_file = tls_file("server.pem"), .private_key_file = tls_file("server-key.pem")};
}

// Named other.example.
io::tls_server_options other_certificate() {
  return {.certificate_file = tls_file("other.pem"), .private_key_file = tls_file("other-key.pem")};
}

http::client_options trusting_the_authority() {
  http::client_options options;

  options.tls.ca_file = tls_file("ca.pem");
  return options;
}

http::router make_router() {
  http::router router;

  router.add(http::verb::get, "/hello", "hello", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = "hello";
    co_return response;
  });
  router.get("/pieces", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{4} * 1024, 'x');

      for (auto index = 0; index < 256; ++index) {
        co_await out.write(piece);
      }
    };
    return response;
  });
  router.post("/refuse", [](http::request, http::body_reader) {
    http::response response;

    response.status = http::status::content_too_large;
    response.body = "too large";
    return response;
  });
  router.add(http::verb::post, "/echo", "echo", [](http::request request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = std::move(request.body);
    co_return response;
  });
  router.add(http::verb::get, "/hang", "hang", [](http::request, http::route_match) -> task<http::response> {
    co_await quoll::async::sleep_for(1h);
    co_return http::response{};
  });

  return router;
}

// Counts the connections a server accepts.
struct counting_listener {
  io::any_listener *inner;
  int *accepted;

  [[nodiscard]] task<io::any_stream> accept() {
    auto stream = co_await inner->accept();

    ++*accepted;
    co_return stream;
  }
};

// Runs `call` with the URL of a server presenting `certificate`, and stops the server once it ends. The certificate is
// loaded before anything else: what cannot be loaded throws from here.
template <class T>
task<T> with_server(io::tls_server_options certificate, std::function<task<T>(std::string)> call,
                    http::serve_options options = {}, int *accepted = nullptr) {
  auto const router = make_router();
  auto listener = co_await io::listen_tls(0, std::move(certificate));
  auto const url = "https://127.0.0.1:" + std::to_string(listener.local_port().value());

  // Some handlers hang on purpose, and these tests do not wait for them.
  options.shutdown_timeout = std::chrono::nanoseconds::zero();

  int ignored = 0;
  counting_listener counted{&listener, accepted != nullptr ? accepted : &ignored};
  std::stop_source stop;

  auto const serving = [&]() -> task<> {
    co_await stop_scope<void>{http::server{router, std::move(options)}.serve(std::move(counted)), stop.get_token()};
  };

  auto const calling = [&]() -> task<T> {
    stop_on_exit const stop_server{stop};

    co_return co_await call(url);
  };

  auto [served, result] = co_await when_all(serving(), calling());

  co_return std::move(result);
}

// The body of a GET.
std::function<task<std::string>(std::string)> get_body(http::client_options options, std::string target = "/hello") {
  return [options = std::move(options), target = std::move(target)](std::string url) -> task<std::string> {
    http::client const client{url, options};

    co_return (co_await client.send(make_request(http::verb::get, target))).body;
  };
}

// The error a GET fails with: none if it succeeds.
std::function<task<std::error_code>(std::string)> get_error(http::client_options options,
                                                            std::string target = "/hello") {
  return [options = std::move(options), target = std::move(target)](std::string url) -> task<std::error_code> {
    http::client const client{url, options};

    try {
      static_cast<void>(co_await client.send(make_request(http::verb::get, target)));
    } catch (std::system_error const &error) {
      co_return error.code();
    }

    co_return std::error_code{};
  };
}

// A client that connects to the server's name rather than its address.
task<std::string> get_by_name(std::string url, http::client_options options) {
  auto const port = url.substr(url.rfind(':') + 1);

  co_return co_await get_body(std::move(options))("https://localhost:" + port);
}

// The address of a server that accepts TCP connections and answers nothing, not even a handshake.
task<std::error_code> handshake_with_silent_server(http::client_options options) {
  auto listener = co_await io::listen(0);
  auto const url = "https://127.0.0.1:" + std::to_string(listener.local_port().value());
  auto [accepted, error] = co_await when_all(listener.accept(), get_error(std::move(options))(url));

  co_return error;
}

// A peer that connects over TCP and starts no handshake: how its read ends.
std::function<task<std::error_code>(std::string)> idle_tcp_peer() {
  return [](std::string url) -> task<std::error_code> {
    auto const port = static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    auto connection = co_await io::connect("127.0.0.1", port);
    std::array<char, 16> received{};
    auto const read = co_await connection.read_some(std::as_writable_bytes(std::span{received}));

    co_return read.error;
  };
}

http::serve_options idle_for(std::chrono::nanoseconds timeout) {
  http::serve_options options;

  options.connection.idle_timeout = timeout;
  return options;
}

// A client speaking plain HTTP to a TLS port.
std::function<task<std::error_code>(std::string)> plain_client() {
  return [](std::string url) -> task<std::error_code> {
    url.replace(0, 5, "http");

    co_return co_await get_error({})(std::move(url));
  };
}

// A TLS client of a server speaking plain HTTP.
task<std::error_code> tls_client_of_plain_server() {
  auto const router = make_router();
  auto listener = co_await io::listen(0);
  auto const url = "https://127.0.0.1:" + std::to_string(listener.local_port().value());
  std::stop_source stop;

  auto const serving = [&]() -> task<> {
    co_await stop_scope<void>{http::server{router}.serve(std::move(listener)), stop.get_token()};
  };

  auto const calling = [&]() -> task<std::error_code> {
    stop_on_exit const stop_server{stop};

    co_return co_await get_error(trusting_the_authority())(url);
  };

  auto [served, error] = co_await when_all(serving(), calling());

  co_return error;
}

// A server that reads a request, sends `response`, and closes without TLS's closing message: what the client gets.
task<std::error_code> closing_without_notice(std::string response) {
  auto listener = co_await io::listen_tls(0, server_certificate());
  auto const url = "https://127.0.0.1:" + std::to_string(listener.local_port().value());

  auto const serving = [&]() -> task<> {
    auto stream = co_await listener.accept();
    std::array<char, 1024> received{};

    static_cast<void>(co_await stream.read_some(std::as_writable_bytes(std::span{received})));
    static_cast<void>(co_await io::write(stream, std::as_bytes(std::span{response})));
  };

  auto [served, error] = co_await when_all(serving(), get_error(trusting_the_authority())(url));

  co_return error;
}

// Listens through http::server, as a program does, and gets a page.
task<std::string> listen_and_get(std::uint16_t port) {
  auto const router = make_router();
  std::stop_source stop;

  auto const serving = [&]() -> task<> {
    co_await stop_scope<void>{http::server{router}.listen(port, {.tls = server_certificate()}), stop.get_token()};
  };

  auto const calling = [&]() -> task<std::string> {
    stop_on_exit const stop_server{stop};

    co_return co_await get_body(trusting_the_authority())("https://127.0.0.1:" + std::to_string(port));
  };

  auto [served, body] = co_await when_all(serving(), calling());

  co_return body;
}

} // namespace

TEST_CASE("TLS: a client calls a server, checking its certificate against an authority", "[tls][asio][runtime]") {
  REQUIRE(run_task(with_server<std::string>(server_certificate(), get_body(trusting_the_authority()))) == "hello");
}

TEST_CASE("TLS: http::server serves over TLS with a certificate", "[tls][asio][runtime][server]") {
  REQUIRE(run_task(listen_and_get(free_port())) == "hello");
}

TEST_CASE("TLS: the server is reached by its name", "[tls][asio][runtime]") {
  std::function<task<std::string>(std::string)> const call = [](std::string url) {
    return get_by_name(std::move(url), trusting_the_authority());
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call)) == "hello");
}

TEST_CASE("TLS: requests reuse a connection kept alive", "[tls][asio][runtime]") {
  int accepted = 0;

  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};

    static_cast<void>(co_await client.send(make_request(http::verb::get, "/hello")));
    co_return (co_await client.send(make_request(http::verb::get, "/hello"))).body;
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call, {}, &accepted)) == "hello");
  REQUIRE(accepted == 1);
}

TEST_CASE("TLS: a body read in pieces, on a connection then reused", "[tls][asio][runtime]") {
  int accepted = 0;

  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};
    auto response = co_await client.stream(make_request(http::verb::get, "/pieces"));
    std::size_t size = 0;

    while (auto const piece = co_await response.body.read()) {
      size += piece->size();
    }

    co_return std::to_string(size) + " " + (co_await client.send(make_request(http::verb::get, "/hello"))).body;
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call, {}, &accepted)) == "1048576 hello");
  REQUIRE(accepted == 1);
}

TEST_CASE("TLS: a body_writer: all of it, then a server refusing it", "[tls][asio][runtime]") {
  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};

    auto const upload = [](std::string target, http::body_writer writer) {
      http::request request;

      request.method = http::verb::post;
      request.target = std::move(target);
      request.body_writer = std::move(writer);
      return request;
    };

    auto const whole = co_await client.send(upload("/echo", [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{4} * 1024, 'x');

      for (auto index = 0; index < 256; ++index) {
        co_await out.write(piece);
      }
    }));
    auto const refused = co_await client.send(upload("/refuse", [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{64} * 1024, 'x');

      for (;;) {
        co_await out.write(piece);
      }
    }));

    co_return std::to_string(whole.body.size()) + " " + std::to_string(static_cast<int>(refused.status));
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call)) == "1048576 413");
}

TEST_CASE("TLS: a head that cuts a write short is still a head, whatever becomes of what follows",
          "[tls][asio][runtime]") {
  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};
    http::request request;

    request.method = http::verb::post;
    request.target = "/refuse";
    request.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{64} * 1024, 'x');

      for (;;) {
        co_await out.write(piece);
      }
    };

    auto response = co_await client.stream(std::move(request));

    // Whichever comes: what is promised is the head, and no more than a failure from what follows.
    try {
      while (co_await response.body.read()) {
      }
    } catch (std::system_error const &) {
    }

    co_return std::to_string(static_cast<int>(response.status));
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call)) == "413");
}

TEST_CASE("TLS: a kept connection the server closed is replaced", "[tls][asio][runtime][timer]") {
  int accepted = 0;

  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};

    static_cast<void>(co_await client.send(make_request(http::verb::get, "/hello")));
    co_await quoll::async::sleep_for(100ms);
    co_return (co_await client.send(make_request(http::verb::get, "/hello"))).body;
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call, idle_for(20ms), &accepted)) == "hello");
  REQUIRE(accepted == 2);
}

TEST_CASE("TLS: a response of a known length is complete, however the connection closes", "[tls][asio][runtime]") {
  REQUIRE(run_task(closing_without_notice("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")) ==
          std::error_code{});
}

TEST_CASE("TLS: a response delimited by the end of the stream needs TLS's closing message", "[tls][asio][runtime]") {
  // Anyone on the way could have cut it: it does not pass for complete.
  REQUIRE(run_task(closing_without_notice("HTTP/1.1 200 OK\r\n\r\nmaybe all of it")) == std::errc::connection_reset);
}

namespace {

// What a client gets from a server for `request` and what it still sends after it, and how the connection ended for it.
struct raw_exchange {
  std::string received;
  std::error_code wrote;
  std::error_code ended;
};

std::function<task<raw_exchange>(std::string)> raw_call(std::string request, std::string then = {}) {
  return [request = std::move(request), then = std::move(then)](std::string url) -> task<raw_exchange> {
    auto const port = static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    auto stream = co_await io::connect_tls("127.0.0.1", port, trusting_the_authority().tls);
    raw_exchange result;
    std::array<char, 4096> chunk{};

    result.wrote = (co_await io::write(stream, std::as_bytes(std::span{request}))).error;

    if (!result.wrote && !then.empty()) {
      result.wrote = (co_await io::write(stream, std::as_bytes(std::span{then}))).error;
    }

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        result.ended = read.error;
        co_return result;
      }

      result.received.append(chunk.data(), read.transferred);
    }
  };
}

} // namespace

TEST_CASE("TLS: a server ending a connection sends TLS's closing message", "[tls][asio][runtime]") {
  auto const ended = run_task(with_server<raw_exchange>(
      server_certificate(), raw_call("GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n")));

  REQUIRE(ended.received.starts_with("HTTP/1.1 200"));
  // Not std::errc::connection_reset: this client knows it has it all.
  REQUIRE(ended.ended == io::error::end_of_stream);
}

TEST_CASE("TLS: a connection found idle gets TLS's closing message", "[tls][asio][runtime]") {
  auto const ended = run_task(with_server<raw_exchange>(server_certificate(), raw_call({}), idle_for(20ms)));

  REQUIRE(ended.received.empty());
  REQUIRE(ended.ended == io::error::end_of_stream);
}

TEST_CASE("TLS: a client closing its side gets TLS's closing message back", "[tls][asio][runtime]") {
  auto const call = [](std::string url) -> task<std::error_code> {
    auto const port = static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    auto stream = co_await io::connect_tls("127.0.0.1", port, trusting_the_authority().tls);
    std::string const request = "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n";
    std::array<char, 4096> chunk{};

    (void)co_await io::write(stream, std::as_bytes(std::span{request}));

    // The server has answered once this has come: the connection is then waiting for the next request.
    (void)co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    auto const closed = co_await stream.shutdown_send();

    if (closed.error) {
      co_return closed.error;
    }

    // Once: a second closing message would follow the socket's own end, and nothing goes after it.
    auto const again = co_await stream.shutdown_send();

    if (again.error) {
      co_return again.error;
    }

    if (auto const after = co_await stream.write_some(std::as_bytes(std::span{request}));
        after.error != std::errc::broken_pipe) {
      co_return std::make_error_code(std::errc::protocol_error);
    }

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        co_return read.error;
      }
    }
  };

  REQUIRE(run_task(with_server<std::error_code>(server_certificate(), call)) == io::error::end_of_stream);
}

TEST_CASE("TLS: a stream moved before its handshake and its shutdown run keeps them", "[tls][asio]") {
  ::asio::io_context context;
  quoll::asio::tls_acceptor acceptor{
      ::asio::ip::tcp::acceptor{context, ::asio::ip::tcp::endpoint{::asio::ip::address_v4::loopback(), 0}},
      server_certificate()};
  ::asio::ip::tcp::endpoint const endpoint{::asio::ip::address_v4::loopback(), acceptor.local_port()};
  ::asio::ssl::context client_context{::asio::ssl::context::tls_client};
  std::optional<std::pair<std::error_code, std::error_code>> outcome;
  std::exception_ptr failure;

  client_context.set_verify_mode(::asio::ssl::verify_none);

  auto const work = [&]() -> task<std::pair<std::error_code, std::error_code>> {
    // The server gets the client's closing message as the end of the stream.
    auto const serving = [&]() -> task<std::error_code> {
      auto stream = co_await acceptor.accept();
      std::array<char, 64> received{};

      for (;;) {
        auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{received}));

        if (read.error) {
          co_return read.error;
        }
      }
    };

    auto const calling = [&]() -> task<std::error_code> {
      ::asio::ip::tcp::socket socket{context};

      socket.connect(endpoint);

      quoll::asio::tls_stream stream{quoll::asio::tls_stream::session_type{std::move(socket), client_context}};
      // Each binds the stream's state at the call: it may move before the task runs.
      auto shaking = stream.handshake(quoll::asio::tls_stream::session_type::client);
      quoll::asio::tls_stream moved{std::move(stream)};

      if (auto const error = co_await std::move(shaking)) {
        co_return error;
      }

      auto stopping = moved.shutdown_send();
      quoll::asio::tls_stream const again{std::move(moved)};

      co_return (co_await std::move(stopping)).error;
    };

    auto [server, client] = co_await when_all(serving(), calling());

    co_return std::pair{server, client};
  };

  quoll::async::co_spawn(
      quoll::asio::executor{context.get_executor()}, work(),
      quoll::test::overloaded{[&](std::pair<std::error_code, std::error_code> value) { outcome = value; },
                              [&](std::exception_ptr error) { failure = std::move(error); }});
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }

  REQUIRE(outcome);
  REQUIRE(outcome->first == io::error::end_of_stream);
  REQUIRE_FALSE(outcome->second);
}

TEST_CASE("TLS: an acceptor moved while its accept is pending still accepts", "[tls][asio]") {
  ::asio::io_context context;
  quoll::asio::tls_acceptor acceptor{
      ::asio::ip::tcp::acceptor{context, ::asio::ip::tcp::endpoint{::asio::ip::address_v4::loopback(), 0}},
      server_certificate()};
  auto const port = acceptor.local_port();
  std::optional<std::string> answer;
  std::exception_ptr failure;

  auto const work = [&]() -> task<std::string> {
    auto accepting = acceptor.accept();
    // Its state and its context are bound at the call.
    quoll::asio::tls_acceptor const moved{std::move(acceptor)};

    auto const serving = [&]() -> task<> {
      auto stream = co_await std::move(accepting);
      std::array<char, 16> received{};

      (void)co_await stream.read_some(std::as_writable_bytes(std::span{received}));

      std::string_view const reply = "pong";

      (void)co_await io::write(stream, std::as_bytes(std::span{reply}));
    };

    auto const calling = [&]() -> task<std::string> {
      auto stream = co_await io::connect_tls("127.0.0.1", port, trusting_the_authority().tls);
      std::string_view const ping = "ping";
      std::array<char, 16> received{};

      (void)co_await io::write(stream, std::as_bytes(std::span{ping}));

      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{received}));

      co_return std::string{received.data(), read.transferred};
    };

    auto [served, reply] = co_await when_all(serving(), calling());

    static_cast<void>(served);
    static_cast<void>(moved);
    co_return reply;
  };

  quoll::async::co_spawn(quoll::asio::executor{context.get_executor()}, work(),
                         quoll::test::overloaded{[&](std::string value) { answer = std::move(value); },
                                                 [&](std::exception_ptr error) { failure = std::move(error); }});
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }

  REQUIRE(answer == "pong");
}

TEST_CASE("TLS: a client still sending a refused body gets its answer and the end of it", "[tls][asio][runtime]") {
  http::serve_options options;

  options.connection.limits.max_body_size = 16;

  auto const ended = run_task(with_server<raw_exchange>(
      server_certificate(),
      raw_call("POST /hello HTTP/1.1\r\nHost: h\r\nContent-Length: 4194304\r\n\r\n", std::string(4 * 1024 * 1024, 'x')),
      std::move(options)));

  REQUIRE_FALSE(ended.wrote);
  REQUIRE(ended.received.starts_with("HTTP/1.1 413"));
  REQUIRE(ended.ended == io::error::end_of_stream);
}

TEST_CASE("TLS: a body larger than a TLS record goes both ways", "[tls][asio][runtime]") {
  std::string const sent(1024 * 1024, 'x');

  std::function<task<std::string>(std::string)> const call = [&sent](std::string url) -> task<std::string> {
    http::client const client{url, trusting_the_authority()};

    co_return (co_await client.send(make_request(http::verb::post, "/echo", sent))).body;
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call)) == sent);
}

TEST_CASE("TLS: a certificate from an authority not trusted is refused", "[tls][asio][runtime]") {
  // By default, the system's authorities: they do not know the tests' one.
  auto const error = run_task(with_server<std::error_code>(server_certificate(), get_error({})));

  REQUIRE(error);
  REQUIRE(error != std::errc::operation_canceled);
}

TEST_CASE("TLS: a certificate naming another server is refused", "[tls][asio][runtime]") {
  auto const error = run_task(with_server<std::error_code>(other_certificate(), get_error(trusting_the_authority())));

  REQUIRE(error);
  REQUIRE(error != std::errc::operation_canceled);
}

TEST_CASE("TLS: verify_peer = false accepts any certificate", "[tls][asio][runtime]") {
  http::client_options options;

  options.tls.verify_peer = false;

  REQUIRE(run_task(with_server<std::string>(other_certificate(), get_body(options))) == "hello");
}

TEST_CASE("TLS: an authority file that cannot be read fails the request", "[tls][asio][runtime]") {
  http::client_options options;

  options.tls.ca_file = tls_file("no-such-file.pem");

  REQUIRE(run_task(with_server<std::error_code>(server_certificate(), get_error(options))));
}

TEST_CASE("TLS: a server needs both a certificate and its key, valid", "[tls][asio][runtime]") {
  auto const only = [](std::string certificate, std::string key) {
    return run_task(with_server<std::error_code>(
        {.certificate_file = std::move(certificate), .private_key_file = std::move(key)}, get_error({})));
  };

  REQUIRE_THROWS_AS(only(tls_file("server.pem"), ""), std::invalid_argument);
  REQUIRE_THROWS_AS(only("", tls_file("server-key.pem")), std::invalid_argument);
  REQUIRE_THROWS_AS(only(tls_file("no-such-file.pem"), tls_file("server-key.pem")), std::system_error);
  REQUIRE_THROWS_AS(only(tls_file("server.pem"), tls_file("no-such-file.pem")), std::system_error);
  REQUIRE_THROWS_AS(only(tls_file("server.pem"), tls_file("other-key.pem")), std::system_error);
}

TEST_CASE("TLS: a peer that starts no handshake is closed by the idle timeout", "[tls][asio][runtime][timer]") {
  auto const error = run_task(with_server<std::error_code>(server_certificate(), idle_tcp_peer(), idle_for(20ms)));

  REQUIRE(error == io::error::end_of_stream);
}

TEST_CASE("TLS: a slow handshake holds nobody back", "[tls][asio][runtime]") {
  // A peer stays idle before its handshake, with no timeout to end it, while a request goes through.
  std::function<task<std::string>(std::string)> const call = [](std::string url) -> task<std::string> {
    auto const port = static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
    auto idle = co_await io::connect("127.0.0.1", port);

    co_return co_await get_body(trusting_the_authority())(std::move(url));
  };

  REQUIRE(run_task(with_server<std::string>(server_certificate(), call, idle_for(0s))) == "hello");
}

TEST_CASE("TLS: plain HTTP to a TLS port fails, and does not hang", "[tls][asio][runtime]") {
  REQUIRE(run_task(with_server<std::error_code>(server_certificate(), plain_client())));
}

TEST_CASE("TLS: a TLS client of a plain server fails, and does not hang", "[tls][asio][runtime]") {
  REQUIRE(run_task(tls_client_of_plain_server()));
}

TEST_CASE("TLS: the connect timeout covers the handshake", "[tls][asio][runtime][timer]") {
  auto options = trusting_the_authority();

  options.connect_timeout = 20ms;

  REQUIRE(run_task(handshake_with_silent_server(options)) == std::errc::timed_out);
}

TEST_CASE("TLS: the response timeout ends a request", "[tls][asio][runtime][timer]") {
  auto options = trusting_the_authority();

  options.response_timeout = 20ms;

  REQUIRE(run_task(with_server<std::error_code>(server_certificate(), get_error(options, "/hang"))) ==
          std::errc::timed_out);
}

TEST_CASE("TLS: a stop request from another thread cancels a request", "[tls][asio][runtime]") {
  quoll::asio::runtime runtime;
  std::stop_source source;
  std::optional<std::error_code> error;

  co_spawn(runtime.get_executor(),
           with_server<std::error_code>(server_certificate(), get_error(trusting_the_authority(), "/hang")),
           quoll::test::overloaded{[&](std::error_code result) { error = result; }, [](std::exception_ptr) {}},
           source.get_token());

  std::jthread const stopper{[&source] {
    std::this_thread::sleep_for(50ms);
    source.request_stop();
  }};

  runtime.run();

  REQUIRE(error == std::errc::operation_canceled);
}

#else

TEST_CASE("without TLS: what needs it says so", "[tls][asio][runtime]") {
  http::client const client{"https://127.0.0.1:1"};

  auto const send = [&]() -> task<int> {
    static_cast<void>(co_await client.send(make_request(http::verb::get, "/")));
    co_return 0;
  };

  auto const listen = []() -> task<int> {
    http::router const router;

    co_await http::server{router}.listen(free_port(), {.tls = {.certificate_file = "cert", .private_key_file = "key"}});
    co_return 0;
  };

  REQUIRE_THROWS_AS(run_task(send()), std::logic_error);
  REQUIRE_THROWS_AS(run_task(listen()), std::logic_error);
}

#endif
