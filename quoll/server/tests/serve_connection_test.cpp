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
#include <quoll/async/when_all.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::verb;
using quoll::async::task;

http::router make_router() {
  http::router router;

  router.add(verb::get, "/hello", "hello", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = "hello";
    co_return response;
  });
  router.add(verb::post, "/echo", "echo", [](http::request request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = std::move(request.body);
    co_return response;
  });
  router.add(verb::get, "/fail", "fail", [](http::request, http::route_match) -> task<http::response> {
    throw std::runtime_error{"handler failure"};
  });
  router.add(verb::get, "/chunked", "chunked", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.fields.add("Transfer-Encoding", "chunked");
    response.body = "in chunks";
    co_return response;
  });
  router.add(verb::get, "/bye", "bye", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.fields.add("Connection", "keep-alive, Close");
    response.body = "bye";
    co_return response;
  });
  router.add(verb::get, "/invalid", "invalid", [](http::request, http::route_match) -> task<http::response> {
    // A 204 cannot have a body: the serializer refuses it.
    http::response response;

    response.status = http::status::no_content;
    response.body = "not allowed";
    co_return response;
  });
  return router;
}

struct exchanged {
  std::vector<http::response> responses;
  // Whether the server closed the connection after the last one.
  bool closed = false;
};

// The client end: sends `raw` at once, reads a response per entry of `methods` (the method of the request it answers),
// then whether the server closed. Owns its end: the server sees the end of the stream when this returns.
task<exchanged> exchange(io::memory_stream client, std::string raw, std::vector<verb> methods, bool expect_close) {
  exchanged result;
  std::string buffer;

  (void)co_await io::write(client, std::as_bytes(std::span{raw}));

  for (auto const method : methods) {
    http::response_parser parser;

    parser.set_request_method(method);

    if (auto const error = co_await http::read(client, buffer, parser)) {
      co_return result;
    }

    result.responses.push_back(parser.release());
  }

  if (expect_close) {
    std::byte byte{};
    auto const received = co_await client.read_some(std::span{&byte, 1});

    result.closed = received.error == io::error::end_of_stream;
  }

  co_return result;
}

exchanged serve(std::string raw, std::vector<verb> methods, bool expect_close = false,
                quoll::server::connection_options const &options = {}) {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  auto outcome = quoll::test::run(
      quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                             exchange(std::move(client), std::move(raw), std::move(methods), expect_close)));

  return std::move(std::get<1>(outcome.value()));
}

using namespace std::chrono_literals;

using quoll::server::connection_options;
using quoll::test::virtual_time;

// What the peer reads until the server closes, as it arrives.
task<std::string> read_until_closed(io::memory_stream &client) {
  std::string received;
  std::array<char, 256> chunk;

  for (;;) {
    auto const read = co_await client.read_some(std::as_writable_bytes(std::span{chunk}));

    if (read.error) {
      co_return received;
    }

    received.append(chunk.data(), read.transferred);
  }
}

// Sends `first` and `second` with `delay` between them, then reads until the server closes. Empty parts are not sent.
task<std::string> peer(io::memory_stream client, std::string first, std::chrono::nanoseconds delay,
                       std::string second) {
  if (!first.empty()) {
    (void)co_await io::write(client, std::as_bytes(std::span{first}));
  }

  if (!second.empty()) {
    co_await quoll::async::sleep_for(delay);
    (void)co_await io::write(client, std::as_bytes(std::span{second}));
  }

  co_return co_await read_until_closed(client);
}

// A peer sends a byte of `raw` every `pace`, until the server closes. Reads what it answered, once it no longer
// accepts.
task<std::string> trickle(io::memory_stream client, std::string raw, std::chrono::nanoseconds pace) {
  auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
    for (auto const &byte : raw) {
      if ((co_await io::write(client, std::as_bytes(std::span{&byte, 1}))).error) {
        co_return;
      }

      co_await quoll::async::sleep_for(pace);
    }
  }());

  co_return received;
}

// The whole session with `peer_task`, on a virtual clock: what the peer read.
template <class MakePeer>
std::string session(virtual_time &time, connection_options const &options, MakePeer make_peer) {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  auto outcome =
      quoll::test::run(time, quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                                                    make_peer(std::move(client))));

  return std::move(std::get<1>(outcome.value()));
}

// A peer that stays until it ends by itself: its end closes after `delay`.
task<std::string> leave_after(io::memory_stream, std::chrono::nanoseconds delay) {
  co_await quoll::async::sleep_for(delay);
  co_return std::string{};
}

// A stream whose writes wait until they are stopped: a peer that no longer reads.
class stuck_writes {
public:
  explicit stuck_writes(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    try {
      co_await quoll::async::sleep_for(1h);
    } catch (std::system_error const &error) {
      co_return io::io_result{error.code(), 0};
    }

    co_return io::io_result{};
  }

private:
  io::memory_stream inner_;
};

static_assert(io::ByteStream<stuck_writes>);

// A request that starts, then a stream that fails with `failure`; what is written goes to `written`.
class failing_reads {
public:
  failing_reads(std::error_code failure, std::string &written) noexcept : failure_(failure), written_(&written) {}

  [[nodiscard]] task<io::io_result> read_some(std::span<std::byte> buffer) {
    if (started_) {
      co_return io::io_result{failure_, 0};
    }

    started_ = true;
    buffer[0] = std::byte{'G'};
    co_return io::io_result{std::error_code{}, 1};
  }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    written_->append(reinterpret_cast<char const *>(data.data()), data.size());
    co_return io::io_result{std::error_code{}, data.size()};
  }

private:
  std::error_code failure_;
  std::string *written_;
  bool started_ = false;
};

static_assert(io::ByteStream<failing_reads>);

// serve_connection() with a peer that sends `raw` and no longer reads: its time on the clock.
std::chrono::nanoseconds time_until_stuck_writes_end(connection_options const &options) {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::string const raw = "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(stuck_writes{std::move(server)}, router, options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  return time.now();
}

} // namespace

TEST_CASE("serve_connection: requests get their responses, on a connection kept alive", "[server]") {
  auto const result = serve("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
                            "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nping",
                            {verb::get, verb::post});

  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.responses[0].status == http::status::ok);
  REQUIRE(result.responses[0].body == "hello");
  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE_FALSE(result.responses[0].fields.contains("Connection"));
  REQUIRE(result.responses[1].body == "ping");
}

TEST_CASE("serve_connection: on_request reports each request that was answered", "[server]") {
  auto [client, server] = io::make_memory_stream_pair();
  http::router router;
  quoll::server::connection_options options;
  std::size_t reported = 0;

  router.add(verb::get, "/hello", "hello", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = "hello";
    co_return response;
  });
  options.on_request = [&](quoll::server::request_summary const &summary) {
    REQUIRE(summary.target == "/hello");
    REQUIRE(summary.status == http::status::ok);
    ++reported;
  };

  auto const session = quoll::server::serve_connection(std::move(server), router, options);

  static_cast<void>(session);
  static_cast<void>(reported);
}

TEST_CASE("serve_connection: a request that does not keep the connection alive ends it", "[server]") {
  auto const closing = serve("GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", {verb::get}, true);

  REQUIRE(closing.responses.front().fields.find("Connection") == "close");
  REQUIRE(closing.closed);

  auto const old = serve("GET /hello HTTP/1.0\r\n\r\n", {verb::get}, true);

  REQUIRE(old.responses.front().body == "hello");
  REQUIRE(old.closed);
}

TEST_CASE("serve_connection: HEAD announces the length of a body it does not send", "[server]") {
  auto const result =
      serve("HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n", {verb::head, verb::get});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE(result.responses[0].body.empty());
  // The connection stays in step: the next response is read whole.
  REQUIRE(result.responses[1].body == "hello");
}

TEST_CASE("serve_connection: what cannot be read answers its status and ends the connection", "[server]") {
  auto const garbage = serve("NOT HTTP\r\n\r\n", {verb::get}, true);

  REQUIRE(garbage.responses.front().status == http::status::bad_request);
  REQUIRE(garbage.responses.front().fields.find("Connection") == "close");
  REQUIRE(garbage.closed);
  REQUIRE(serve("PROPFIND /x HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, true).responses.front().status ==
          http::status::not_implemented);

  quoll::server::connection_options const small{.limits = {.max_header_size = 64, .max_body_size = 4},
                                                .on_handler_error = {}};
  auto const long_header = serve("GET /hello HTTP/1.1\r\nHost: x\r\nX-Long: " + std::string(100, 'a') + "\r\n\r\n",
                                 {verb::get}, true, small);

  REQUIRE(long_header.responses.front().status == http::status::request_header_fields_too_large);
  REQUIRE(long_header.responses.front().reason == "Request Header Fields Too Large");
  REQUIRE(serve("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123456789", {verb::post}, true, small)
              .responses.front()
              .status == http::status::content_too_large);
}

TEST_CASE("serve_connection: a handler that fails answers 500, and the connection goes on", "[server]") {
  auto const result = serve("GET /fail HTTP/1.1\r\nHost: x\r\n\r\n"
                            "GET /invalid HTTP/1.1\r\nHost: x\r\n\r\n"
                            "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                            {verb::get, verb::get, verb::get});

  REQUIRE(result.responses[0].status == http::status::internal_server_error);
  REQUIRE(result.responses[1].status == http::status::internal_server_error);
  REQUIRE(result.responses[2].body == "hello");
}

TEST_CASE("serve_connection: the router's own answers go out like any response", "[server]") {
  auto const result = serve("DELETE /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /nowhere HTTP/1.1\r\nHost: x\r\n\r\n",
                            {verb::delete_, verb::get});

  REQUIRE(result.responses[0].status == http::status::method_not_allowed);
  REQUIRE(result.responses[0].fields.find("Allow") == "GET, HEAD");
  REQUIRE(result.responses[1].status == http::status::not_found);
}

TEST_CASE("serve_connection: HEAD on a chunked response sends a length, not the terminating chunk", "[server]") {
  auto const result =
      serve("HEAD /chunked HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n", {verb::head, verb::get});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "9");
  REQUIRE_FALSE(result.responses[0].fields.contains("Transfer-Encoding"));
  // No stray terminating chunk: the next response is read whole.
  REQUIRE(result.responses[1].body == "hello");
}

TEST_CASE("serve_connection: a handler's Connection: close ends the connection", "[server]") {
  auto const result = serve("GET /bye HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, true);

  REQUIRE(result.responses.front().body == "bye");
  REQUIRE(result.closed);
}

TEST_CASE("serve_connection: what a handler throws goes to on_handler_error", "[server]") {
  std::string reported;
  quoll::server::connection_options options;

  options.on_handler_error = [&](std::exception_ptr failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      reported = error.what();
    }
  };

  REQUIRE(serve("GET /fail HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, false, options).responses.front().status ==
          http::status::internal_server_error);
  REQUIRE(reported == "handler failure");
}

TEST_CASE("serve_connection: a connection waiting for its first request closes silently", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = 5s},
                                [](auto client) { return peer(std::move(client), "", 0s, ""); });

  REQUIRE(received.empty());
  REQUIRE(time.now() == 5s);
}

TEST_CASE("serve_connection: a connection idle after a response closes silently", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = 5s}, [](auto client) {
    return peer(std::move(client), "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n", 0s, "");
  });

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE(received.ends_with("hello"));
  REQUIRE(time.now() == 5s);
}

TEST_CASE("serve_connection: a request that does not arrive in time answers 408 and closes", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.read_timeout = 3s}, [](auto client) {
    return peer(std::move(client), "GET /hello HTTP/1.1\r\nHo", 0s, "");
  });

  REQUIRE(received.starts_with("HTTP/1.1 408"));
  REQUIRE(received.contains("Connection: close"));
  REQUIRE(time.now() == 3s);
}

TEST_CASE("serve_connection: the read timeout covers a request as a whole, however it trickles", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.read_timeout = 3s}, [](auto client) {
    return trickle(std::move(client), "GET /" + std::string(1000, 'a'), 1s);
  });

  REQUIRE(received.starts_with("HTTP/1.1 408"));
  REQUIRE(time.now() >= 3s);
  REQUIRE(time.now() <= 4s);
}

TEST_CASE("serve_connection: the idle timeout is not the read timeout", "[server][timer]") {
  virtual_time time;

  // The time to wait for a request is not counted in the time it takes to arrive, and the other way round.
  auto const received = session(time, connection_options{.idle_timeout = 2s, .read_timeout = 10s}, [](auto client) {
    return peer(std::move(client), "GET /hello HTTP/1.1\r\n", 5s, "Host: h\r\n\r\n");
  });

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE(time.now() == 7s);
}

TEST_CASE("serve_connection: a pipelined request is not idle", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = 2s}, [](auto client) {
    return peer(std::move(client), "GET /hello HTTP/1.1\r\nHost: h\r\n\r\nGET /hello HTTP/1.1\r\nHost: h\r\n\r\n", 0s,
                "");
  });

  REQUIRE(received.find("HTTP/1.1 200") != received.rfind("HTTP/1.1 200"));
  REQUIRE(time.now() == 2s);
}

TEST_CASE("serve_connection: a response that does not leave in time ends the connection", "[server][timer]") {
  REQUIRE(time_until_stuck_writes_end(connection_options{.write_timeout = 4s}) == 4s);
}

TEST_CASE("serve_connection: empty lines after a request are no request", "[server][timer]") {
  virtual_time time;

  // Without a read timeout left to end it: the idle timeout has to.
  auto const received = session(time, connection_options{.idle_timeout = 5s, .read_timeout = 0s}, [](auto client) {
    return peer(std::move(client), "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nhi\r\n", 0s, "");
  });

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE_FALSE(received.contains("408"));
  REQUIRE(time.now() == 5s);
}

TEST_CASE("serve_connection: a peer sending only empty lines stays idle", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = 5s, .read_timeout = 0s},
                                [](auto client) { return peer(std::move(client), "\r\n\r\n", 1s, "\r\n"); });

  REQUIRE(received.empty());
  REQUIRE(time.now() == 5s);
}

TEST_CASE("serve_connection: empty lines before a request are skipped", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = 5s}, [](auto client) {
    return peer(std::move(client), "\r\n\r\nGET /hello HTTP/1.1\r\nHost: h\r\n\r\n", 0s, "");
  });

  REQUIRE(received.starts_with("HTTP/1.1 200"));
}

TEST_CASE("serve_connection: only its own timeout answers 408", "[server][timer]") {
  auto const router = make_router();

  // Reported by the stream, not by a timeout: a peer gone (ETIMEDOUT), a stream cancelled by someone else.
  auto const failure = GENERATE(std::errc::timed_out, std::errc::operation_canceled);
  std::string written;
  virtual_time time;

  auto const serve_it = [&]() -> task<int> {
    co_await quoll::server::serve_connection(failing_reads{std::make_error_code(failure), written}, router);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, serve_it()) == 1);
  REQUIRE(written.empty());
}

TEST_CASE("serve_connection: the default timeouts apply", "[server][timer]") {
  virtual_time idle;
  virtual_time read;

  REQUIRE(session(idle, {}, [](auto client) { return peer(std::move(client), "", 0s, ""); }).empty());
  REQUIRE(idle.now() == 60s);
  REQUIRE(session(read, {}, [](auto client) {
            return peer(std::move(client), "GET", 0s, "");
          }).starts_with("HTTP/1.1 408"));
  REQUIRE(read.now() == 30s);
  REQUIRE(time_until_stuck_writes_end({}) == 30s);
}

TEST_CASE("serve_connection: a negative timeout has expired, rather than lifting the limit", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.idle_timeout = -1s},
                                [](auto client) { return peer(std::move(client), "", 0s, ""); });

  REQUIRE(received.empty());
  REQUIRE(time.now() == 0s);
}

TEST_CASE("serve_connection: a timeout of zero does not apply", "[server][timer]") {
  virtual_time time;

  // The peer's own departure ends the connection, long after the defaults.
  auto const received = session(time, connection_options{.idle_timeout = 0s, .read_timeout = 0s},
                                [](auto client) { return leave_after(std::move(client), 5h); });

  REQUIRE(received.empty());
  REQUIRE(time.now() == 5h);
}

TEST_CASE("serve_connection: a handler takes the time it needs", "[server][timer]") {
  virtual_time time;
  auto router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  router.add(verb::get, "/slow", "slow", [](http::request, http::route_match) -> task<http::response> {
    co_await quoll::async::sleep_for(1h);

    http::response response;

    response.body = "done";
    co_return response;
  });

  auto outcome = quoll::test::run(
      time, quoll::async::when_all(quoll::server::serve_connection(
                                       std::move(server), router,
                                       connection_options{.idle_timeout = 1s, .read_timeout = 1s, .write_timeout = 1s}),
                                   peer(std::move(client), "GET /slow HTTP/1.1\r\nHost: h\r\n\r\n", 0s, "")));

  REQUIRE(std::get<1>(outcome.value()).ends_with("done"));
  REQUIRE(time.now() == 1h + 1s);
}

TEST_CASE("serve_connection: a stop request is no timeout", "[server][timer]") {
  virtual_time time;
  std::stop_source source;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  auto const stop_soon = [&]() -> task<> {
    co_await quoll::async::sleep_for(1s);
    source.request_stop();
  };

  // A request under way: no 408 for it.
  auto outcome = quoll::test::run(time,
                                  quoll::async::when_all(quoll::server::serve_connection(std::move(server), router),
                                                         peer(std::move(client), "GET /hello HTTP/1.1\r\nHo", 0s, ""),
                                                         stop_soon()),
                                  source.get_token());

  REQUIRE(std::get<1>(outcome.value()).empty());
  REQUIRE(time.now() == 1s);
}
