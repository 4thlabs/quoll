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

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
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
  auto const upload = [](http::body_reader body) -> task<http::response> {
    http::response response;

    while (auto const piece = co_await body.read()) {
      response.body += *piece;
    }

    co_return response;
  };

  router.post("/upload", upload);
  router.put("/upload/{name}", [](std::string name, http::body_reader body) -> task<http::response> {
    http::response response;

    response.body = std::move(name) + ":";

    while (auto const piece = co_await body.read()) {
      response.body += *piece;
    }

    co_return response;
  });
  // Answers without reading what it was sent.
  router.post("/refuse", [](http::body_reader) {
    http::response response;

    response.status = http::status::forbidden;
    return response;
  });
  // Reads a piece, then answers, whatever the rest is.
  router.post("/first", [](http::body_reader body) -> task<http::response> {
    http::response response;

    response.body = (co_await body.read()).value_or("nothing");
    co_return response;
  });
  auto const writing = [](std::string length, std::vector<std::string> pieces, bool then_throw = false) {
    return [length = std::move(length), pieces = std::move(pieces), then_throw](http::request) {
      http::response response;

      if (!length.empty()) {
        response.fields.add("Content-Length", length);
      }

      response.body_writer = [pieces, then_throw](http::body_stream out) -> task<> {
        for (auto const &piece : pieces) {
          co_await out.write(piece);
        }

        if (then_throw) {
          throw std::runtime_error{"the writer failed"};
        }
      };
      return response;
    };
  };

  router.get("/in-chunks", writing({}, {"ping", "", "pong"}));
  router.get("/sized", writing("8", {"ping", "pong"}));
  router.get("/too-short", writing("8", {"ping"}));
  router.get("/too-long", writing("4", {"ping", "pong"}));
  router.get("/writer-throws", writing({}, {"ping"}, true));
  router.get("/writer-fails-first", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream) -> task<> {
      throw std::runtime_error{"cannot open what it writes"};
      co_return;
    };
    return response;
  });
  router.get("/empty-with-length", writing("8", {}));
  router.get("/http-1-0-response", [](http::request) {
    http::response response;

    response.version = http::version::http_1_0;
    response.body_writer = [](http::body_stream out) -> task<> { co_await out.write("ping"); };
    return response;
  });
  router.get("/with-trailers", [](http::request) {
    http::response response;

    response.fields.add("Transfer-Encoding", "chunked");
    response.trailers.add("X-Sum", "1");
    response.body_writer = [](http::body_stream out) -> task<> { co_await out.write("ping"); };
    return response;
  });
  router.get("/no-content-written", [](http::request) {
    http::response response;

    response.status = http::status::no_content;
    response.body_writer = [](http::body_stream) -> task<> { co_return; };
    return response;
  });
  router.get("/two-bodies", [](http::request) {
    http::response response;

    response.body = "whole";
    response.body_writer = [](http::body_stream) -> task<> { co_return; };
    return response;
  });
  // Reads, then fails on its own.
  router.post("/throwing", [](http::body_reader body) -> task<http::response> {
    static_cast<void>(co_await body.read());
    throw std::logic_error{"a bug of its own"};
  });
  // Gives up on a body that takes too long, and answers.
  router.post("/patient", [](http::body_reader body) -> task<http::response> {
    http::response response;

    try {
      while (co_await quoll::async::with_timeout(std::chrono::seconds{1}, body.read())) {
      }
    } catch (std::system_error const &thrown) {
      REQUIRE(thrown.code() == std::errc::timed_out);
      response.status = http::status::gateway_timeout;
    }

    // That ended the reading: a stream does not take it up again.
    REQUIRE_THROWS_AS(co_await body.read(), std::system_error);

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

  // Stuck too: once a write has failed, it must not be tried, or it takes another write timeout.
  [[nodiscard]] task<io::io_result> shutdown_send() { return write_some({}); }

private:
  io::memory_stream inner_;
};

static_assert(io::HalfClosableStream<stuck_writes>);

// A stream whose shutdown waits until it is stopped, whatever else it does.
class stuck_shutdown {
public:
  explicit stuck_shutdown(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] auto write_some(std::span<std::byte const> data) { return inner_.write_some(data); }

  [[nodiscard]] task<io::io_result> shutdown_send() {
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

static_assert(io::HalfClosableStream<stuck_shutdown>);

// A plain ByteStream, with no way to stop sending.
class stuck_free_stream {
public:
  explicit stuck_free_stream(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) noexcept { return inner_.read_some(buffer); }

  [[nodiscard]] auto write_some(std::span<std::byte const> data) noexcept { return inner_.write_some(data); }

private:
  io::memory_stream inner_;
};

static_assert(io::ByteStream<stuck_free_stream>);
static_assert(!io::HalfClosableStream<stuck_free_stream>);

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
std::chrono::nanoseconds
time_until_stuck_writes_end(connection_options const &options,
                            std::string const &raw = "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(stuck_writes{std::move(server)}, router, options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  return time.now();
}

// serve_connection() on a stream that writes, but whose shutdown is stuck: its time on the clock.
std::chrono::nanoseconds time_until_stuck_shutdown_ends(connection_options const &options) {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::string const raw = "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(stuck_shutdown{std::move(server)}, router, options);
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

namespace {

// What such a client received: each status in turn, and the body of the last response.
struct answers {
  std::vector<http::status> statuses;
  std::string body;
};

// A client that sends its head, waits for what the server answers to it, then sends its body.
task<answers> send_body_after_answer(io::memory_stream client, std::string head, std::string body) {
  answers received;
  std::string buffer;

  (void)co_await io::write(client, std::as_bytes(std::span{head}));

  for (;;) {
    http::response_parser parser;

    if (co_await http::read(client, buffer, parser)) {
      co_return received;
    }

    auto response = parser.release();

    received.statuses.push_back(response.status);

    if (response.status != http::status::continue_) {
      received.body = std::move(response.body);
      co_return received;
    }

    (void)co_await io::write(client, std::as_bytes(std::span{body}));
  }
}

// What a server answers such a client.
answers answers_to(std::string head, std::string body, connection_options const &options = {}) {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  auto outcome = quoll::test::run(
      quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                             send_body_after_answer(std::move(client), std::move(head), std::move(body))));

  return std::move(std::get<1>(outcome.value()));
}

} // namespace

TEST_CASE("serve_connection: Expect: 100-continue gets its answer, then the body", "[server]") {
  std::vector const asked_then_answered{http::status::continue_, http::status::ok};

  SECTION("as the client waits for it") {
    auto const got =
        answers_to("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-Continue\r\n\r\n", "ping");

    REQUIRE(got.statuses == asked_then_answered);
    REQUIRE(got.body == "ping");
  }

  SECTION("not asked for: nothing comes before the response") {
    auto const got = answers_to("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nping", {});

    REQUIRE(got.statuses == std::vector{http::status::ok});
    REQUIRE(got.body == "ping");
  }

  SECTION("part of the body came with the head") {
    auto const got =
        answers_to("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\npi", "ng");

    REQUIRE(got.statuses == asked_then_answered);
    REQUIRE(got.body == "ping");
  }

  SECTION("with a chunked body") {
    auto const got =
        answers_to("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
                   "4\r\nping\r\n0\r\n\r\n");

    REQUIRE(got.statuses == asked_then_answered);
    REQUIRE(got.body == "ping");
  }

  SECTION("as a list, which may name it twice") {
    auto const got = answers_to(
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue, 100-continue,\r\n\r\n", "ping");

    REQUIRE(got.statuses == asked_then_answered);
    REQUIRE(got.body == "ping");
  }

  SECTION("refused for its size: the final answer comes instead") {
    connection_options const small{.limits = {.max_body_size = 2}, .on_handler_error = {}};
    auto const got = answers_to("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n",
                                "ping", small);

    REQUIRE(got.statuses == std::vector{http::status::content_too_large});
  }
}

TEST_CASE("serve_connection: what Expect does in other cases", "[server]") {
  SECTION("the body came with the head: no need for it") {
    auto const result =
        serve("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\nping", {verb::post});

    REQUIRE(result.responses.front().status == http::status::ok);
    REQUIRE(result.responses.front().body == "ping");
  }

  SECTION("no body: no need for it") {
    auto const result = serve("GET /hello HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n", {verb::get});

    REQUIRE(result.responses.front().status == http::status::ok);
  }

  SECTION("anything else is refused, and ends the connection") {
    auto const result = serve("GET /hello HTTP/1.1\r\nHost: x\r\nExpect: something\r\n\r\n", {verb::get}, true);

    REQUIRE(result.responses.front().status == http::status::expectation_failed);
    REQUIRE(result.responses.front().reason == "Expectation Failed");
    REQUIRE(result.closed);
  }

  SECTION("one of two fields being anything else is enough") {
    auto const result =
        serve("GET /hello HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nExpect: else\r\n\r\n", {verb::get}, true);

    REQUIRE(result.responses.front().status == http::status::expectation_failed);
  }

  SECTION("an empty field asks for nothing") {
    auto const result = serve("GET /hello HTTP/1.1\r\nHost: x\r\nExpect:\r\n\r\n", {verb::get});

    REQUIRE(result.responses.front().status == http::status::ok);
  }

  SECTION("HTTP/1.0 cannot have asked for it: ignored") {
    auto const result = serve("GET /hello HTTP/1.0\r\nHost: x\r\nExpect: something\r\n\r\n", {verb::get});

    REQUIRE(result.responses.front().status == http::status::ok);
  }
}

TEST_CASE("serve_connection: a middleware's response goes out, and what it throws answers 500", "[server]") {
  auto router = make_router();

  router.use([](http::request message, http::next next) -> task<http::response> {
    if (message.target == "/broken") {
      throw std::runtime_error{"middleware"};
    }

    auto answer = co_await next(std::move(message));

    answer.fields.set("X-Frame-Options", "DENY");
    co_return answer;
  });

  int reported = 0;
  connection_options const options{.on_handler_error = [&reported](std::exception_ptr const &) { ++reported; }};
  auto [client, server] = io::make_memory_stream_pair();
  auto outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router, options),
      exchange(
          std::move(client),
          "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\nGET /broken HTTP/1.1\r\nHost: x\r\n\r\n",
          {verb::get, verb::get}, false)));
  auto const &responses = std::get<1>(outcome.value()).responses;

  REQUIRE(responses.at(0).fields.find("X-Frame-Options") == "DENY");
  REQUIRE(responses.at(1).status == http::status::internal_server_error);
  // Built past the middlewares, so that this one added nothing, and reported.
  REQUIRE_FALSE(responses.at(1).fields.contains("X-Frame-Options"));
  REQUIRE(reported == 1);
}

TEST_CASE("serve_connection: a request refused as it is read reaches no middleware", "[server]") {
  auto router = make_router();
  int seen = 0;

  router.use([&seen](http::request message, http::next next) -> task<http::response> {
    ++seen;
    co_return co_await next(std::move(message));
  });

  auto [client, server] = io::make_memory_stream_pair();
  auto outcome =
      quoll::test::run(quoll::async::when_all(quoll::server::serve_connection(std::move(server), router),
                                              exchange(std::move(client), "NOT HTTP\r\n\r\n", {verb::get}, true)));

  REQUIRE(std::get<1>(outcome.value()).responses.at(0).status == http::status::bad_request);
  REQUIRE(seen == 0);
}

TEST_CASE("serve_connection: HEAD through a middleware still has no body", "[server]") {
  auto router = make_router();

  router.use([](http::request message, http::next next) -> task<http::response> {
    auto answer = co_await next(std::move(message));

    answer.fields.set("X-Seen", "yes");
    co_return answer;
  });

  auto [client, server] = io::make_memory_stream_pair();
  auto outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router),
      exchange(std::move(client), "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n", {verb::head}, false)));
  auto const &answer = std::get<1>(outcome.value()).responses.at(0);

  REQUIRE(answer.fields.find("X-Seen") == "yes");
  REQUIRE(answer.fields.find("Content-Length") == "5");
  REQUIRE(answer.body.empty());
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

  // With nothing read after the answer, as this peer goes on sending.
  auto const received = session(time, connection_options{.read_timeout = 3s, .lingering_timeout = 0s}, [](auto client) {
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

namespace {

// A peer sending its pieces, one every two seconds, then reading what comes.
task<std::string> pieces_two_seconds_apart(io::memory_stream client, std::vector<std::string> pieces) {
  for (auto const &piece : pieces) {
    (void)co_await io::write(client, std::as_bytes(std::span{piece}));
    co_await quoll::async::sleep_for(2s);
  }

  co_return co_await read_until_closed(client);
}

// A request whose client waits for `100 Continue` before its body.
std::string const expecting = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";

} // namespace

TEST_CASE("serve_connection: the read timeout covers a head and a body together", "[server][timer]") {
  virtual_time time;

  // The head ends at 2 s and the body at 4 s: each is in time, but not both.
  auto const received = session(time, connection_options{.read_timeout = 3s}, [](auto client) {
    return pieces_two_seconds_apart(std::move(client),
                                    {"POST /echo HTTP/1.1\r\nHost: h\r\n", "Content-Length: 4\r\n\r\n", "ping"});
  });

  REQUIRE(received.starts_with("HTTP/1.1 408"));
}

TEST_CASE("serve_connection: only 100 Continue starts the read timeout again", "[server][timer]") {
  virtual_time time;

  // The same, but for what the client waits for: the body has its own time after the answer.
  auto const received = session(time, connection_options{.read_timeout = 3s}, [](auto client) {
    return pieces_two_seconds_apart(std::move(client), {"POST /echo HTTP/1.1\r\nHost: h\r\n",
                                                        "Content-Length: 4\r\nExpect: 100-continue\r\n\r\n", "ping"});
  });

  REQUIRE(received.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200"));
}

TEST_CASE("serve_connection: a body that never comes after 100 Continue gets a 408", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.read_timeout = 3s},
                                [](auto client) { return peer(std::move(client), expecting, {}, {}); });

  REQUIRE(received.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 408"));
  REQUIRE(time.now() == 3s);
}

TEST_CASE("serve_connection: HTTP/1.0 waits for no 100 Continue", "[server][timer]") {
  virtual_time time;

  auto const received = session(time, connection_options{.read_timeout = 3s}, [](auto client) {
    return peer(std::move(client),
                "POST /echo HTTP/1.0\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n", 1s, "ping");
  });

  REQUIRE(received.starts_with("HTTP/1.1 200"));
}

TEST_CASE("serve_connection: a 100 Continue that does not leave is under the write timeout", "[server][timer]") {
  // With no read timeout to end it.
  REQUIRE(time_until_stuck_writes_end(connection_options{.read_timeout = 0s, .write_timeout = 4s}, expecting) == 4s);
}

namespace {

// How a connection ends with a peer that sends `request`, reads what comes until the server stops sending, sends `more`
// a second later, and stays connected until `leaves`: when the server had gone, and what the peer read and when.
struct lingered {
  std::chrono::nanoseconds server_gone{};
  std::chrono::nanoseconds answer_ended{};
  std::string received;
};

lingered linger_with(connection_options const &options, std::string request, std::chrono::nanoseconds leaves) {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  lingered result;
  std::string const more(10000, 'x');

  auto const serving = [&](io::memory_stream stream) -> task<int> {
    co_await quoll::server::serve_connection(std::move(stream), router, options);
    result.server_gone = time.now();
    co_return 1;
  };

  auto const peering = [&](io::memory_stream stream) -> task<int> {
    (void)co_await io::write(stream, std::as_bytes(std::span{request}));
    result.received = co_await read_until_closed(stream);
    result.answer_ended = time.now();
    co_await quoll::async::sleep_for(1s);
    (void)co_await io::write(stream, std::as_bytes(std::span{more}));
    co_await quoll::async::sleep_for(leaves - 1s);
    co_return 1;
  };

  auto outcome = quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peering(std::move(client))));

  REQUIRE(outcome);
  return result;
}

// Refused for its size, with its body still to come.
std::string const refused = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 100000\r\n\r\n";

connection_options small_bodies(std::chrono::nanoseconds lingering) {
  connection_options options;

  options.limits.max_body_size = 10;
  options.lingering_timeout = lingering;
  return options;
}

} // namespace

TEST_CASE("serve_connection: a refused request is answered, then what still comes is dropped", "[server][timer]") {
  auto const ended = linger_with(small_bodies(5s), refused, 2s);

  // The peer has its answer and the end of it at once, and the server waits for the peer to leave.
  REQUIRE(ended.received.starts_with("HTTP/1.1 413"));
  REQUIRE(ended.answer_ended == 0s);
  REQUIRE(ended.server_gone == 2s);
}

TEST_CASE("serve_connection: a peer that goes quiet is waited for up to the lingering timeout", "[server][timer]") {
  auto const ended = linger_with(small_bodies(5s), refused, 60s);

  // Its last bytes came at 1 s.
  REQUIRE(ended.received.starts_with("HTTP/1.1 413"));
  REQUIRE(ended.server_gone == 6s);
}

TEST_CASE("serve_connection: a peer that keeps sending is read up to the lingering time", "[server][timer]") {
  virtual_time time;
  auto options = small_bodies(5s);

  options.lingering_time = 3s;

  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::chrono::nanoseconds server_gone{};

  auto const serving = [&](io::memory_stream stream) -> task<int> {
    co_await quoll::server::serve_connection(std::move(stream), router, options);
    server_gone = time.now();
    co_return 1;
  };

  auto const peering = [&](io::memory_stream stream) -> task<int> {
    (void)co_await io::write(stream, std::as_bytes(std::span{refused}));

    for (int second = 0; second < 10; ++second) {
      co_await quoll::async::sleep_for(1s);
      (void)co_await io::write(stream, std::as_bytes(std::span{refused}));
    }

    co_return 1;
  };

  auto outcome = quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peering(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(server_gone == 3s);
}

TEST_CASE("serve_connection: a drain or a stop request ends the lingering", "[server][timer]") {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::stop_source stopping;
  auto options = small_bodies(5s);
  std::chrono::nanoseconds server_gone{};
  auto const stop_with_drain = GENERATE(true, false);

  if (stop_with_drain) {
    options.drain = stopping.get_token();
  }

  auto const serving = [&](io::memory_stream stream) -> task<int> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stop_with_drain ? std::stop_token{} : stopping.get_token()};
    server_gone = time.now();
    co_return 1;
  };

  auto const peering = [&](io::memory_stream stream) -> task<int> {
    (void)co_await io::write(stream, std::as_bytes(std::span{refused}));
    (void)co_await read_until_closed(stream);
    co_await quoll::async::sleep_for(2s);
    stopping.request_stop();
    co_await quoll::async::sleep_for(1s);
    co_return 1;
  };

  auto outcome = quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peering(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(server_gone == 2s);
}

TEST_CASE("serve_connection: no lingering timeout closes at once", "[server][timer]") {
  auto const ended = linger_with(small_bodies(0s), refused, 2s);

  REQUIRE(ended.received.starts_with("HTTP/1.1 413"));
  REQUIRE(ended.server_gone == 0s);
}

TEST_CASE("serve_connection: a last response lingers too, for what is on its way", "[server][timer]") {
  auto const ended = linger_with(small_bodies(5s), "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 2s);

  REQUIRE(ended.received.starts_with("HTTP/1.1 200"));
  REQUIRE(ended.answer_ended == 0s);
  REQUIRE(ended.server_gone == 2s);
}

TEST_CASE("serve_connection: a connection found idle does not linger", "[server][timer]") {
  virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::chrono::nanoseconds server_gone{};
  connection_options const options{.idle_timeout = 3s};

  auto const serving = [&](io::memory_stream stream) -> task<int> {
    co_await quoll::server::serve_connection(std::move(stream), router, options);
    server_gone = time.now();
    co_return 1;
  };

  auto const peering = [&](io::memory_stream stream) -> task<int> {
    (void)co_await read_until_closed(stream);
    co_await quoll::async::sleep_for(10s);
    co_return 1;
  };

  auto outcome = quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peering(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(server_gone == 3s);
}

TEST_CASE("serve_connection: stopping to send is under the write timeout", "[server][timer]") {
  REQUIRE(time_until_stuck_shutdown_ends(connection_options{.write_timeout = 4s}) == 4s);
}

TEST_CASE("serve_connection: without a timer to end it, nothing lingers", "[server]") {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  std::string const request = "NOT HTTP\r\n\r\n";

  // The peer's end stays open throughout: on a run_loop, which has no timer, reading it would never end.
  auto const peering = [&]() -> task<std::string> {
    (void)co_await io::write(client, std::as_bytes(std::span{request}));
    co_return co_await read_until_closed(client);
  };

  auto outcome =
      quoll::test::run(quoll::async::when_all(quoll::server::serve_connection(std::move(server), router), peering()));

  REQUIRE(outcome);
  REQUIRE(std::get<1>(*outcome).starts_with("HTTP/1.1 400"));
}

TEST_CASE("serve_connection: a stream that cannot stop sending just closes", "[server]") {
  auto [client, server] = io::make_memory_stream_pair();
  auto const router = make_router();
  std::string const request = "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<std::string> {
    (void)co_await io::write(client, std::as_bytes(std::span{request}));
    co_await quoll::server::serve_connection(stuck_free_stream{std::move(server)}, router);
    co_return co_await read_until_closed(client);
  };

  REQUIRE(quoll::test::run(answer())->starts_with("HTTP/1.1 200"));
}

TEST_CASE("serve_connection: a handler reads a body in pieces, and the connection goes on", "[server]") {
  connection_options options;

  // Far less than what is sent: the handler holds it, not the connection.
  options.limits.max_body_size = 16;

  std::string const body(std::size_t{64} * 1024, 'x');
  auto const size = std::to_string(body.size());

  SECTION("with a length") {
    auto const got = serve("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: " + size + "\r\n\r\n" + body +
                               "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                           {verb::post, verb::get}, false, options);

    REQUIRE(got.responses.size() == 2);
    REQUIRE(got.responses[0].body == body);
    REQUIRE(got.responses[1].body == "hello");
  }

  SECTION("chunked, with a parameter of the path") {
    auto const got = serve("PUT /upload/a%20b HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "4\r\nping\r\n4\r\npong\r\n0\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                           {verb::put, verb::get}, false, options);

    REQUIRE(got.responses.size() == 2);
    REQUIRE(got.responses[0].body == "a b:pingpong");
    REQUIRE(got.responses[1].body == "hello");
  }

  SECTION("empty") {
    auto const got = serve("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"
                           "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                           {verb::post, verb::get}, false, options);

    REQUIRE(got.responses.at(0).body.empty());
    REQUIRE(got.responses.at(1).body == "hello");
  }

  SECTION("another route keeps the limit of a whole body") {
    auto const got =
        serve("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: " + size + "\r\n\r\n", {verb::post}, true, options);

    REQUIRE(got.responses.at(0).status == http::status::content_too_large);
    REQUIRE(got.closed);
  }
}

TEST_CASE("serve_connection: what a handler leaves unread ends the connection after its response", "[server]") {
  auto const refused = serve("POST /refuse HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\npi", {verb::post}, true);

  REQUIRE(refused.responses.at(0).status == http::status::forbidden);
  REQUIRE(refused.responses.at(0).fields.find("Connection") == "close");
  REQUIRE(refused.closed);

  auto const partly = serve("POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\nping", {verb::post}, true);

  REQUIRE(partly.responses.at(0).body == "ping");
  REQUIRE(partly.closed);
}

TEST_CASE("serve_connection: all of a body came with its head, unread: the connection goes on", "[server]") {
  auto const got = serve("POST /refuse HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi"
                         "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                         {verb::post, verb::get});

  REQUIRE(got.responses.at(0).status == http::status::forbidden);
  REQUIRE(got.responses.at(1).body == "hello");
}

namespace {

// A head, then the rest once the handler is reading, as what comes with the head is refused before it runs: a whole
// message in one read reaches no handler if its body is at fault.
task<http::response> send_in_two(io::memory_stream stream, std::string head, std::string rest) {
  (void)co_await io::write(stream, std::as_bytes(std::span{head}));

  for (auto turn = 0; turn < 32; ++turn) {
    co_await quoll::async::yield();
  }

  (void)co_await io::write(stream, std::as_bytes(std::span{rest}));

  std::string buffer;
  http::response_parser parser;

  (void)co_await http::read(stream, buffer, parser);
  co_return parser.release();
}

} // namespace

TEST_CASE("serve_connection: a body in pieces past its limit", "[server]") {
  connection_options options;

  options.limits.max_streamed_body_size = 4;

  SECTION("announced: refused at its head") {
    auto const got =
        serve("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", {verb::post}, true, options);

    REQUIRE(got.responses.at(0).status == http::status::content_too_large);
    REQUIRE(got.closed);
  }
}

TEST_CASE("serve_connection: what stops the reading of a body is no failure of the handler", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };
  options.limits.max_streamed_body_size = 4;

  // Reaches the handler: past it, what is at fault comes through it.
  auto const through_a_handler = [&](std::string rest) {
    auto started = false;
    http::router router;

    router.post("/reading", [&started](http::body_reader body) -> task<http::response> {
      started = true;

      while (co_await body.read()) {
      }

      co_return http::response{};
    });

    auto [client, server] = io::make_memory_stream_pair();
    auto const outcome = quoll::test::run(quoll::async::when_all(
        quoll::server::serve_connection(std::move(server), router, options),
        send_in_two(std::move(client), "POST /reading HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                    std::move(rest))));

    REQUIRE(started);
    return std::get<1>(outcome.value()).status;
  };

  SECTION("too large, through the handler") {
    REQUIRE(through_a_handler("5\r\nhello\r\n0\r\n\r\n") == http::status::content_too_large);
    REQUIRE(failures == 0);
  }

  SECTION("what is no HTTP") {
    options.limits.max_streamed_body_size = 1024;

    REQUIRE(through_a_handler("ZZ\r\n") == http::status::bad_request);
    REQUIRE(failures == 0);
  }

  SECTION("the peer closing before the end: nothing to answer") {
    options.limits.max_streamed_body_size = 1024;

    auto const router = make_router();
    auto [client, server] = io::make_memory_stream_pair();

    // Owns its end: it goes when this ends, with the body cut short, and what is written after that goes nowhere.
    auto const peer = [](io::memory_stream stream) -> task<bool> {
      std::string const sent = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\nping";

      co_return !(co_await io::write(stream, std::as_bytes(std::span{sent}))).error;
    };

    auto const outcome = quoll::test::run(quoll::async::when_all(
        quoll::server::serve_connection(std::move(server), router, options), peer(std::move(client))));

    REQUIRE(outcome);
    REQUIRE(std::get<1>(*outcome));
    REQUIRE(failures == 0);
  }
}

TEST_CASE("serve_connection: Expect: 100-continue waits for the handler to ask for the body", "[server]") {
  SECTION("read: the interim response comes first, then the body") {
    auto const got =
        answers_to("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n", "ping");

    REQUIRE(got.statuses == std::vector{http::status::continue_, http::status::ok});
    REQUIRE(got.body == "ping");
  }

  SECTION("refused: the client does not have to send it") {
    auto const got = serve("POST /refuse HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n",
                           {verb::post}, true);

    REQUIRE(got.responses.at(0).status == http::status::forbidden);
    REQUIRE(got.closed);
  }
}

TEST_CASE("serve_connection: with no minimum rate, a body in pieces has a read timeout per piece", "[server][timer]") {
  quoll::test::virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 0;

  // Each piece comes in time, for longer than one timeout in all, and the last never does.
  auto const peer = [](io::memory_stream stream) -> task<http::response> {
    std::string const head = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n";

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));

    for (auto index = 0; index < 3; ++index) {
      co_await quoll::async::sleep_for(2s);

      std::string_view const part = "p";

      (void)co_await io::write(stream, std::as_bytes(std::span{part}));
    }

    std::string buffer;
    http::response_parser parser;

    (void)co_await http::read(stream, buffer, parser);
    co_return parser.release();
  };

  auto outcome =
      quoll::test::run(time, quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                                                    peer(std::move(client))));

  REQUIRE(std::get<1>(outcome.value()).status == http::status::request_timeout);
  // Three pieces two seconds apart, and the timeout after the last.
  REQUIRE(time.now() >= 9s);
}

TEST_CASE("serve_connection: what a handler throws on its own, reading a body, is its failure", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto const got =
      serve("POST /throwing HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nping", {verb::post}, false, options);

  REQUIRE(got.responses.at(0).status == http::status::internal_server_error);
  REQUIRE(failures == 1);
}

TEST_CASE("serve_connection: a handler giving up on a body answers, and the connection ends", "[server][timer]") {
  quoll::test::virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  auto const peer = [](io::memory_stream stream) -> task<http::response> {
    std::string const head = "POST /patient HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n";

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));

    std::string buffer;
    http::response_parser parser;

    (void)co_await http::read(stream, buffer, parser);
    co_return parser.release();
  };

  auto outcome =
      quoll::test::run(time, quoll::async::when_all(quoll::server::serve_connection(std::move(server), router),
                                                    peer(std::move(client))));
  auto const &response = std::get<1>(outcome.value());

  REQUIRE(response.status == http::status::gateway_timeout);
  REQUIRE(response.fields.find("Connection") == "close");
}

TEST_CASE("serve_connection: a read that fails goes on failing, after what came before it", "[server]") {
  std::vector<std::string> seen;
  http::router router;

  // Reads on, past a failure, and answers something else than what the failure asks.
  router.post("/stubborn", [&seen](http::body_reader body) -> task<http::response> {
    for (auto attempt = 0; attempt < 2; ++attempt) {
      try {
        while (auto const piece = co_await body.read()) {
          seen.emplace_back(*piece);
        }
      } catch (std::system_error const &thrown) {
        seen.push_back(thrown.code() == http::error::body_too_large ? "too large" : "another failure");
      }
    }

    http::response response;

    response.body = "what the handler made of it";
    co_return response;
  });

  connection_options options;

  options.limits.max_streamed_body_size = 4;

  auto [client, server] = io::make_memory_stream_pair();
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router, options),
      // In one read: a chunk that fits, and one past the limit.
      send_in_two(std::move(client), "POST /stubborn HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                  "4\r\nping\r\n1\r\nx\r\n0\r\n\r\n")));

  REQUIRE(outcome);

  auto const &response = std::get<1>(*outcome);

  REQUIRE(seen == std::vector<std::string>{"ping", "too large", "too large"});
  // The refusal goes out, in the place of what it made of it.
  REQUIRE(response.status == http::status::content_too_large);
  REQUIRE(response.body.empty());
  REQUIRE(response.fields.find("Connection") == "close");
}

TEST_CASE("serve_connection: a peer gone when the body is asked for", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  auto const peer = [](io::memory_stream stream) -> task<bool> {
    std::string const sent = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";

    co_return !(co_await io::write(stream, std::as_bytes(std::span{sent}))).error;
  };

  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router, options), peer(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(failures == 0);
}

TEST_CASE("serve_connection: a stop during the reading of a body is no failure of the handler", "[server][timer]") {
  quoll::test::virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  auto failures = 0;
  std::stop_source stopping;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto const peer = [&](io::memory_stream stream) -> task<bool> {
    std::string const sent = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\nping";

    (void)co_await io::write(stream, std::as_bytes(std::span{sent}));
    co_await quoll::async::sleep_for(1s);
    stopping.request_stop();

    // Nothing comes: not a 500 for a handler that let the stop through.
    std::array<std::byte, 64> received{};

    co_return (co_await stream.read_some(received)).transferred == 0;
  };

  auto const serving = [&](io::memory_stream stream) -> task<bool> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stopping.get_token()};
    co_return true;
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(std::get<1>(*outcome));
  REQUIRE(failures == 0);
}

TEST_CASE("serve_connection: a response written in pieces", "[server]") {
  auto const next_request = std::string{"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"};

  SECTION("in chunks, and the connection goes on") {
    auto const got = serve("GET /in-chunks HTTP/1.1\r\nHost: x\r\n\r\n" + next_request, {verb::get, verb::get});

    REQUIRE(got.responses.at(0).status == http::status::ok);
    REQUIRE(got.responses.at(0).body == "pingpong");
    REQUIRE(got.responses.at(0).fields.find("Transfer-Encoding") == "chunked");
    REQUIRE_FALSE(got.responses.at(0).fields.contains("Content-Length"));
    REQUIRE(got.responses.at(1).body == "hello");
  }

  SECTION("with a length") {
    auto const got = serve("GET /sized HTTP/1.1\r\nHost: x\r\n\r\n" + next_request, {verb::get, verb::get});

    REQUIRE(got.responses.at(0).body == "pingpong");
    REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
    REQUIRE(got.responses.at(1).body == "hello");
  }

  SECTION("with trailers") {
    auto const got = serve("GET /with-trailers HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get});

    REQUIRE(got.responses.at(0).body == "ping");
    REQUIRE(got.responses.at(0).trailers.find("X-Sum") == "1");
  }

  SECTION("to HTTP/1.0: up to the end of the connection") {
    auto const got = serve("GET /in-chunks HTTP/1.0\r\n\r\n", {verb::get});

    REQUIRE(got.responses.at(0).body == "pingpong");
    REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
    REQUIRE(got.responses.at(0).fields.find("Connection") == "close");
  }

  SECTION("to HEAD: the writer does not run, and a length it announces stays") {
    auto const got = serve("HEAD /in-chunks HTTP/1.1\r\nHost: x\r\n\r\nHEAD /sized HTTP/1.1\r\nHost: x\r\n\r\n",
                           {verb::head, verb::head});

    REQUIRE(got.responses.at(0).status == http::status::ok);
    REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
    REQUIRE(got.responses.at(1).fields.find("Content-Length") == "8");
    REQUIRE(got.responses.at(1).body.empty());
  }
}

TEST_CASE("serve_connection: a response in pieces that cannot be whole is cut, and looks it", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  // What the client gets when the connection ends before the end of the body.
  auto const cut_short = [&](std::string target) {
    auto const got = serve("GET " + std::move(target) + " HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, false, options);

    return got.responses.empty();
  };

  SECTION("less than its length") {
    REQUIRE(cut_short("/too-short"));
    REQUIRE(failures == 1);
  }

  SECTION("a writer that throws after its first piece") {
    REQUIRE(cut_short("/writer-throws"));
    REQUIRE(failures == 1);
  }

  SECTION("more than its length: what it announced has left, and the fault is the writer's") {
    auto const got = serve("GET /too-long HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, true, options);

    REQUIRE(got.responses.at(0).body == "ping");
    REQUIRE(got.closed);
    REQUIRE(failures == 1);
  }
}

TEST_CASE("serve_connection: a response in pieces refused before it starts answers 500", "[server]") {
  auto const target = GENERATE("/no-content-written", "/two-bodies");
  auto const got = serve(std::string{"GET "} + target + " HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get});

  REQUIRE(got.responses.at(0).status == http::status::internal_server_error);
}

TEST_CASE("serve_connection: a writer failing before its first byte answers 500", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto const got = serve("GET /writer-fails-first HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                         {verb::get, verb::get}, false, options);

  REQUIRE(got.responses.at(0).status == http::status::internal_server_error);
  REQUIRE(got.responses.at(1).body == "hello");
  REQUIRE(failures == 1);
}

TEST_CASE("serve_connection: to HTTP/1.0, a body in pieces has no chunks and no trailers", "[server]") {
  auto const got = serve("GET /with-trailers HTTP/1.0\r\n\r\n", {verb::get});

  REQUIRE(got.responses.at(0).status == http::status::ok);
  REQUIRE(got.responses.at(0).body == "ping");
  REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
  REQUIRE(got.responses.at(0).fields.find("Connection") == "close");
}

TEST_CASE("serve_connection: to HEAD, a writer announces no length it does not say", "[server]") {
  auto const got = serve("HEAD /in-chunks HTTP/1.1\r\nHost: x\r\n\r\nHEAD /two-bodies HTTP/1.1\r\nHost: x\r\n\r\n"
                         "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                         {verb::head, verb::head, verb::get});

  REQUIRE_FALSE(got.responses.at(0).fields.contains("Content-Length"));
  REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
  // Nothing of a body follows it, and the next response reads as one.
  REQUIRE(got.responses.at(2).body == "hello");
}

TEST_CASE("serve_connection: a writer reads the request as it writes the response", "[server]") {
  http::router router;

  router.post("/echo-in-pieces", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream out) -> task<> {
      while (auto const piece = co_await body.read()) {
        co_await out.write(*piece);
      }
    };
    return response;
  });

  auto [client, server] = io::make_memory_stream_pair();
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router),
      send_in_two(std::move(client), "POST /echo-in-pieces HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\n",
                  "pingpong")));

  REQUIRE(outcome);

  auto const &response = std::get<1>(*outcome);

  REQUIRE(response.body == "pingpong");
}

TEST_CASE("serve_connection: a stop in a writer is no failure of the writer", "[server][timer]") {
  virtual_time time;
  http::router router;

  router.get("/forever", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      for (;;) {
        co_await out.write("tick");
        co_await quoll::async::sleep_for(1s);
      }
    };
    return response;
  });

  auto failures = 0;
  std::stop_source stopping;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto [client, server] = io::make_memory_stream_pair();

  auto const serving = [&](io::memory_stream stream) -> task<bool> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stopping.get_token()};
    co_return true;
  };

  auto const peer = [&](io::memory_stream stream) -> task<bool> {
    std::string const sent = "GET /forever HTTP/1.1\r\nHost: x\r\n\r\n";

    (void)co_await io::write(stream, std::as_bytes(std::span{sent}));
    co_await quoll::async::sleep_for(3s);
    stopping.request_stop();
    co_return true;
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(failures == 0);
}

TEST_CASE("serve_connection: a response in pieces has a write timeout, not its writer", "[server][timer]") {
  SECTION("a slow writer goes on, past one timeout in all") {
    virtual_time time;
    http::router router;

    router.get("/slowly", [](http::request) {
      http::response response;

      response.body_writer = [](http::body_stream out) -> task<> {
        for (auto index = 0; index < 3; ++index) {
          co_await quoll::async::sleep_for(2s);
          co_await out.write("part");
        }
      };
      return response;
    });

    auto [client, server] = io::make_memory_stream_pair();
    connection_options options;

    options.write_timeout = 3s;

    auto const peer = [](io::memory_stream stream) -> task<http::response> {
      std::string const sent = "GET /slowly HTTP/1.1\r\nHost: x\r\n\r\n";

      (void)co_await io::write(stream, std::as_bytes(std::span{sent}));

      std::string buffer;
      http::response_parser parser;

      (void)co_await http::read(stream, buffer, parser);
      co_return parser.release();
    };

    auto outcome = quoll::test::run(
        time, quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                                     peer(std::move(client))));

    REQUIRE(std::get<1>(outcome.value()).body == "partpartpart");
  }

  SECTION("a peer that stops reading ends it within the timeout") {
    connection_options options;

    options.write_timeout = 3s;

    auto failures = 0;

    options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

    auto const ended_at = time_until_stuck_writes_end(options, "GET /in-chunks HTTP/1.1\r\nHost: h\r\n\r\n");

    REQUIRE(ended_at == 3s);
    REQUIRE(failures == 0);
  }
}

TEST_CASE("serve_connection: a body_writer sent through http::write is refused", "[server]") {
  http::response response;
  auto [client, server] = io::make_memory_stream_pair();

  response.body_writer = [](http::body_stream) -> task<> { co_return; };
  REQUIRE_THROWS_AS(http::write(server, response), std::invalid_argument);
}

TEST_CASE("serve_connection: 100 Continue does not follow a response that has started", "[server]") {
  http::router router;

  router.post("/preamble-first", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream out) -> task<> {
      co_await out.write("pre:");

      while (auto const piece = co_await body.read()) {
        co_await out.write(*piece);
      }
    };
    return response;
  });

  auto [client, server] = io::make_memory_stream_pair();
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router),
      send_in_two(std::move(client),
                  "POST /preamble-first HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n",
                  "ping")));

  REQUIRE(outcome);

  auto const &response = std::get<1>(*outcome);

  // What the client reads first is the final response, and its body reads whole.
  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.body == "pre:ping");
}

TEST_CASE("serve_connection: a writer that writes nothing of its length answers 500", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto const got = serve("GET /empty-with-length HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, false, options);

  REQUIRE(got.responses.at(0).status == http::status::internal_server_error);
  REQUIRE(failures == 1);
}

TEST_CASE("serve_connection: a writer finding the request at fault before it starts answers as a refusal", "[server]") {
  http::router router;

  router.post("/echo-in-pieces", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream out) -> task<> {
      while (auto const piece = co_await body.read()) {
        co_await out.write(*piece);
      }
    };
    return response;
  });

  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto [client, server] = io::make_memory_stream_pair();
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router, options),
      send_in_two(std::move(client), "POST /echo-in-pieces HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                  "ZZ\r\n")));

  REQUIRE(outcome);
  REQUIRE(std::get<1>(*outcome).status == http::status::bad_request);
  REQUIRE(failures == 0);
}

TEST_CASE("serve_connection: a writer's own HTTP/1.0 response has no chunks either", "[server]") {
  auto const got = serve("GET /http-1-0-response HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get});

  REQUIRE(got.responses.at(0).status == http::status::ok);
  REQUIRE(got.responses.at(0).body == "ping");
  REQUIRE_FALSE(got.responses.at(0).fields.contains("Transfer-Encoding"));
}

TEST_CASE("serve_connection: a writer making nothing of a failed request does not make it a success", "[server]") {
  http::router router;

  // Swallows what it read and what stopped it, and returns.
  router.post("/swallowing", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream) -> task<> {
      try {
        while (co_await body.read()) {
        }
      } catch (std::system_error const &) {
      }
    };
    return response;
  });

  auto [client, server] = io::make_memory_stream_pair();
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router),
      send_in_two(std::move(client), "POST /swallowing HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                  "ZZ\r\n")));

  REQUIRE(outcome);
  REQUIRE(std::get<1>(*outcome).status == http::status::bad_request);
}

TEST_CASE("serve_connection: a writer that a stop ends early has no whole body to end", "[server][timer]") {
  virtual_time time;
  http::router router;

  // Keeps an eye on the stop token, and returns well before its end.
  router.get("/polite", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      co_await out.write("tick");

      auto const stop_token = co_await quoll::async::current_stop_token();

      while (!stop_token.stop_requested()) {
        try {
          co_await quoll::async::sleep_for(1s);
        } catch (std::system_error const &) {
        }
      }
    };
    return response;
  });

  auto failures = 0;
  std::stop_source stopping;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };

  auto [client, server] = io::make_memory_stream_pair();

  auto const serving = [&](io::memory_stream stream) -> task<bool> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stopping.get_token()};
    co_return true;
  };

  auto const peer = [&](io::memory_stream stream) -> task<std::string> {
    std::string const sent = "GET /polite HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string received;
    std::array<char, 1024> chunk{};

    (void)co_await io::write(stream, std::as_bytes(std::span{sent}));
    co_await quoll::async::sleep_for(3s);
    stopping.request_stop();

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        co_return received;
      }

      received.append(chunk.data(), read.transferred);
    }
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client))));

  REQUIRE(outcome);
  // No last chunk: a body cut short.
  REQUIRE_FALSE(std::get<1>(*outcome).contains("0\r\n\r\n"));
  REQUIRE(failures == 0);
}

TEST_CASE("serve_connection: an empty piece sends the head at once", "[server][timer]") {
  virtual_time time;
  http::router router;

  router.get("/announced", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      co_await out.write("");
      co_await quoll::async::sleep_for(10s);
      co_await out.write("late");
    };
    return response;
  });

  auto [client, server] = io::make_memory_stream_pair();
  std::chrono::nanoseconds head_at{};

  auto const peer = [&](io::memory_stream stream) -> task<std::string> {
    std::string const sent = "GET /announced HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    std::string buffer;
    http::response_parser parser;

    (void)co_await io::write(stream, std::as_bytes(std::span{sent}));
    (void)co_await http::read_head(stream, buffer, parser);
    head_at = time.now();
    (void)co_await http::read(stream, buffer, parser);
    co_return parser.release().body;
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(quoll::server::serve_connection(std::move(server), router),
                                                    peer(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(head_at == 0s);
  REQUIRE(std::get<1>(*outcome) == "late");
}

TEST_CASE("serve_connection: a framing refused at the end of its head reaches no handler", "[server]") {
  // A last coding that is not chunked, on a request: what the parser refuses once the head is read.
  auto const got = serve("POST /refuse HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", {verb::post}, true);

  REQUIRE(got.responses.at(0).status == http::status::bad_request);
}

TEST_CASE("serve_connection: what is at fault after a good piece is refused, if the handler reads no more",
          "[server]") {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();

  // It reads a piece, and answers: the rest of what came with that piece is no HTTP.
  auto const outcome = quoll::test::run(quoll::async::when_all(
      quoll::server::serve_connection(std::move(server), router),
      send_in_two(std::move(client), "POST /first HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                  "4\r\nping\r\nZ\r\n")));

  REQUIRE(outcome);
  REQUIRE(std::get<1>(*outcome).status == http::status::bad_request);
}

namespace {

// `body` a part at a time, one `pace` apart, as a POST to `target` of the length it says, or of more if `whole` is
// false: what the server answered, up to its end.
task<std::string> send_in_parts(io::memory_stream client, std::string target, std::size_t size, std::size_t part,
                                std::chrono::nanoseconds pace, bool whole = true) {
  auto const head = "POST " + target +
                    " HTTP/1.1\r\nHost: h\r\nContent-Length: " + std::to_string(whole ? size : size * 2) + "\r\n\r\n";
  auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
    (void)co_await io::write(client, std::as_bytes(std::span{head}));

    std::string const bytes(part, 'x');

    for (std::size_t done = 0; done < size; done += part) {
      if ((co_await io::write(client, std::as_bytes(std::span{bytes}))).error) {
        co_return;
      }

      co_await quoll::async::sleep_for(pace);
    }
  }());

  co_return received;
}

http::router make_paced_router() {
  auto router = make_router();

  router.post("/pieces", [](http::body_reader body) -> task<http::response> {
    std::size_t size = 0;

    while (auto const piece = co_await body.read()) {
      size += piece->size();
    }

    http::response response;

    response.body = std::to_string(size);
    co_return response;
  });
  router.post("/thinking", [](http::body_reader body) -> task<http::response> {
    std::size_t size = 0;

    while (auto const piece = co_await body.read()) {
      size += piece->size();
      co_await quoll::async::sleep_for(10s);
    }

    http::response response;

    response.body = std::to_string(size);
    co_return response;
  });
  router.get("/some", [](http::request) {
    http::response response;

    response.body = std::string(std::size_t{2500}, 'x');
    return response;
  });
  router.get("/some-in-pieces", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      co_await out.write(std::string(std::size_t{2500}, 'x'));
    };
    return response;
  });
  router.get("/events", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      for (auto index = 0; index < 2000; ++index) {
        co_await out.write("twenty bytes of data");
      }
    };
    return response;
  });
  router.get("/large", [](http::request) {
    http::response response;

    response.body = std::string(std::size_t{20000}, 'x');
    return response;
  });
  router.get("/writer", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{2000}, 'x');

      for (auto index = 0; index < 10; ++index) {
        co_await out.write(piece);
      }
    };
    return response;
  });
  router.get("/pondering", [](http::request) {
    http::response response;

    response.body_writer = [](http::body_stream out) -> task<> {
      for (auto index = 0; index < 3; ++index) {
        co_await quoll::async::sleep_for(10s);
        co_await out.write("part");
      }
    };
    return response;
  });
  return router;
}

// A session with a router of its own, on a virtual clock: what the peer read, and when the server was done with it, as
// the peer may wait longer. Nothing lingers.
struct paced_outcome {
  std::string received;
  std::chrono::nanoseconds ended{};
};

template <class MakePeer>
paced_outcome paced_session(virtual_time &time, connection_options options, MakePeer make_peer) {
  auto const router = make_paced_router();
  auto [client, server] = io::make_memory_stream_pair();
  paced_outcome out;

  options.lingering_timeout = 0s;

  auto const serving = [&](io::memory_stream stream) -> task<> {
    co_await quoll::server::serve_connection(std::move(stream), router, options);
    out.ended = time.now();
  };

  auto outcome =
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), make_peer(std::move(client))));

  out.received = std::move(std::get<1>(outcome.value()));
  return out;
}

// A peer that takes what is written `part` bytes at a time, one a second.
class slow_reader {
public:
  slow_reader(io::memory_stream inner, std::size_t part, std::size_t &taken, std::chrono::nanoseconds pause = 1s,
              std::chrono::nanoseconds stall = {}, int every = 0) noexcept
      : inner_(std::move(inner)), part_(part), taken_(&taken), pause_(pause), stall_(stall), every_(every) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    try {
      co_await quoll::async::sleep_for(every_ != 0 && ++writes_ % every_ == 0 ? stall_ : pause_);
    } catch (std::system_error const &error) {
      co_return io::io_result{error.code(), 0};
    }

    auto const done = co_await inner_.write_some(data.first(std::min(part_, data.size())));

    *taken_ += done.transferred;
    co_return done;
  }

private:
  io::memory_stream inner_;
  std::size_t part_;
  std::size_t *taken_;
  std::chrono::nanoseconds pause_;
  std::chrono::nanoseconds stall_;
  int every_;
  int writes_ = 0;
};

// What serve_connection() got out for `target` to a peer taking 2000 bytes a second, and its time on the clock.
struct written {
  std::size_t taken = 0;
  std::chrono::nanoseconds time{};
};

written written_to_a_slow_reader(connection_options const &options, std::string const &target) {
  virtual_time time;
  auto const router = make_paced_router();
  auto [client, server] = io::make_memory_stream_pair();
  written out;
  auto const raw = "GET " + target + " HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(slow_reader{std::move(server), 2000, out.taken}, router, options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  out.time = time.now();
  return out;
}

} // namespace

TEST_CASE("serve_connection: a request has as long as it keeps coming at the minimum rate", "[server][timer]") {
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});

  SECTION("above it, far past the timeout") {
    virtual_time time;

    auto const got = paced_session(
        time, options, [&](auto client) { return send_in_parts(std::move(client), target, 20000, 2000, 1s); });

    REQUIRE(got.received.starts_with("HTTP/1.1 200"));
    REQUIRE(got.ended >= 9s);
  }

  SECTION("below it, it runs out of time, whichever way the body is read") {
    virtual_time time;

    auto const got = paced_session(
        time, options, [&](auto client) { return send_in_parts(std::move(client), target, 20000, 100, 1s); });

    REQUIRE(got.received.starts_with("HTTP/1.1 408"));
    REQUIRE(got.ended >= 3s);
    REQUIRE(got.ended <= 4s);
  }

  SECTION("a silence ends it within the timeout, whatever came before it") {
    virtual_time time;

    auto const got = paced_session(
        time, options, [&](auto client) { return send_in_parts(std::move(client), target, 20000, 20000, 1h, false); });

    REQUIRE(got.received.starts_with("HTTP/1.1 408"));
    REQUIRE(got.ended == 3s);
  }
}

TEST_CASE("serve_connection: what came in several bursts does not pay for a silence either", "[server][timer]") {
  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  // Each part earns less than the timeout, and more than any of them does together.
  auto const got = paced_session(
      time, options, [&](auto client) { return send_in_parts(std::move(client), target, 10000, 2000, 1ms, false); });

  REQUIRE(got.received.starts_with("HTTP/1.1 408"));
  REQUIRE(got.ended >= 3s);
  REQUIRE(got.ended <= 3s + 10ms);
}

TEST_CASE("serve_connection: with no minimum rate, a request has its read timeout as a whole", "[server][timer]") {
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 0;

  auto const got = paced_session(
      time, options, [&](auto client) { return send_in_parts(std::move(client), "/echo", 20000, 2000, 1s); });

  REQUIRE(got.received.starts_with("HTTP/1.1 408"));
  REQUIRE(got.ended == 3s);
}

TEST_CASE("serve_connection: the time a handler takes between two reads does not count against its peer",
          "[server][timer]") {
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  auto const got = paced_session(
      time, options, [&](auto client) { return send_in_parts(std::move(client), "/thinking", 4000, 2000, 11s); });

  REQUIRE(got.received.starts_with("HTTP/1.1 200"));
  REQUIRE(got.ended >= 10s);
}

TEST_CASE("serve_connection: a response has as long as it keeps leaving at the minimum rate", "[server][timer]") {
  connection_options options;

  options.write_timeout = 3s;

  auto const target = GENERATE(std::string{"/large"}, std::string{"/writer"});

  SECTION("above it, far past the timeout") {
    options.min_write_rate = 1000;

    auto const out = written_to_a_slow_reader(options, target);

    REQUIRE(out.taken > 20000);
    REQUIRE(out.time >= 10s);
  }

  SECTION("below it, it runs out of time") {
    options.min_write_rate = 5000;

    auto const out = written_to_a_slow_reader(options, target);

    REQUIRE(out.taken < 20000);
    REQUIRE(out.time >= 3s);
    REQUIRE(out.time <= 7s);
  }
}

TEST_CASE("serve_connection: with no minimum rate, a response has its write timeout", "[server][timer]") {
  connection_options options;

  options.write_timeout = 3s;
  options.min_write_rate = 0;

  SECTION("as a whole") {
    auto const out = written_to_a_slow_reader(options, "/large");

    REQUIRE(out.taken < 20000);
    REQUIRE(out.time == 3s);
  }

  SECTION("per piece") {
    auto const out = written_to_a_slow_reader(options, "/writer");

    REQUIRE(out.taken > 20000);
  }
}

TEST_CASE("serve_connection: the time a body_writer takes between two writes does not count against its peer",
          "[server][timer]") {
  connection_options options;

  options.write_timeout = 3s;
  options.min_write_rate = 1000;

  auto const out = written_to_a_slow_reader(options, "/pondering");

  REQUIRE(out.taken > 12);
  REQUIRE(out.time >= 30s);
}

TEST_CASE("serve_connection: each request of a connection has all of its time", "[server][timer]") {
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 0;

  auto const twice = [](io::memory_stream client) -> task<std::string> {
    auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
      for (std::string_view const close : {"", "Connection: close\r\n"}) {
        auto const first = std::string{"GET /hello HTTP/1.1\r\nHost: h\r\n"} + std::string{close};
        std::string_view const rest = "\r\n";

        (void)co_await io::write(client, std::as_bytes(std::span{first}));
        co_await quoll::async::sleep_for(2s);
        (void)co_await io::write(client, std::as_bytes(std::span{rest}));
      }
    }());

    co_return received;
  };

  auto const got = paced_session(time, options, twice);

  REQUIRE(got.received.starts_with("HTTP/1.1 200"));
  REQUIRE(got.received.find("HTTP/1.1 200", 1) != std::string::npos);
}

TEST_CASE("serve_connection: each response of a connection has all of its time", "[server][timer]") {
  auto const target = GENERATE(std::string{"/some"}, std::string{"/some-in-pieces"});
  virtual_time time;
  auto const router = make_paced_router();
  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;
  std::size_t taken = 0;

  // Well below the rate: one response takes most of the timeout, and two would run out of it.
  options.write_timeout = 3s;
  options.min_write_rate = 10000;

  auto const raw = "GET " + target + " HTTP/1.1\r\nHost: h\r\n\r\nGET " + target +
                   " HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(slow_reader{std::move(server), 2000, taken}, router, options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  REQUIRE(taken > 5000);
}

TEST_CASE("serve_connection: a timeout near all that nanoseconds can hold is one still", "[server][timer]") {
  virtual_time time;
  connection_options options;

  options.read_timeout = std::chrono::nanoseconds::max();
  options.min_read_rate = 1000;

  auto const got = paced_session(
      time, options, [&](auto client) { return send_in_parts(std::move(client), "/pieces", 20000, 2000, 1ms); });

  REQUIRE(got.received.starts_with("HTTP/1.1 200"));
}

TEST_CASE("serve_connection: a read stopped once its patience has run out is stopped, not late", "[server][timer]") {
  virtual_time time;
  auto router = make_paced_router();

  router.post("/stopped", [](http::body_reader body) -> task<http::response> {
    std::stop_source stop;
    http::response response;

    stop.request_stop();

    try {
      static_cast<void>(
          co_await quoll::async::detail::stop_scope<std::optional<std::string_view>>{body.read(), stop.get_token()});
    } catch (std::system_error const &thrown) {
      response.body = thrown.code() == std::errc::operation_canceled ? "stopped" : "another failure";
    }

    co_return response;
  });

  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;

  // Spent from the start: what ends a read here has to be seen for what it is.
  options.read_timeout = -1s;
  options.lingering_timeout = 0s;

  auto const peer = [](io::memory_stream stream) -> task<std::string> {
    std::string const head = "POST /stopped HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nConnection: close\r\n\r\n";

    (void)co_await io::write(stream, std::as_bytes(std::span{head}));
    co_return co_await read_until_closed(stream);
  };

  auto const serving = [&](io::memory_stream stream) -> task<> {
    // The head first, for the operation that reads it not to wait.
    co_await quoll::async::yield();
    co_await quoll::server::serve_connection(std::move(stream), router, options);
  };

  auto outcome = quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client))));
  auto const &received = std::get<1>(outcome.value());

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE(received.ends_with("stopped"));
}

TEST_CASE("serve_connection: what came earns its time before the next read waits", "[server][timer]") {
  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  // Each part is late for what the head left, and pays for the wait for the next as soon as it has come.
  auto const late_parts = [&](io::memory_stream client) -> task<std::string> {
    auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
      auto const head = "POST " + target + " HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 8000\r\n\r\n";
      std::string const part(std::size_t{4000}, 'x');

      (void)co_await io::write(client, std::as_bytes(std::span{head}));

      for (auto index = 0; index < 2; ++index) {
        co_await quoll::async::sleep_for(2s);
        (void)co_await io::write(client, std::as_bytes(std::span{part}));
      }
    }());

    co_return received;
  };

  auto const got = paced_session(time, options, late_parts);

  REQUIRE(got.received.starts_with("HTTP/1.1 200"));
}

TEST_CASE("serve_connection: the framing of a body earns a peer no time", "[server][timer]") {
  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});
  auto const framing =
      GENERATE(std::string{"1;"} + std::string(std::size_t{2000}, 'a'), std::string(std::size_t{2000}, '0'));
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  // Well above the rate, for ever, and not a byte of body.
  auto const endless = [&](io::memory_stream client) -> task<std::string> {
    auto const head = "POST " + target + " HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
      (void)co_await io::write(client, std::as_bytes(std::span{head}));

      for (auto turn = 0; turn < 20; ++turn) {
        if ((co_await io::write(client, std::as_bytes(std::span{framing}))).error) {
          co_return;
        }

        co_await quoll::async::sleep_for(1s);
      }
    }());

    co_return received;
  };

  auto const got = paced_session(time, options, endless);

  REQUIRE(got.received.starts_with("HTTP/1.1 408"));
  REQUIRE(got.ended <= 4s);
}

TEST_CASE("serve_connection: a head has its read timeout as a whole, whatever the rate", "[server][timer]") {
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1;

  auto const got = paced_session(time, options, [](io::memory_stream client) {
    return trickle(std::move(client),
                   "GET /hello HTTP/1.1\r\nX-Slowly: " + std::string(std::size_t{100}, 'a') + "\r\n\r\n", 1s);
  });

  REQUIRE(got.received.starts_with("HTTP/1.1 408"));
  REQUIRE(got.ended == 3s);
}

TEST_CASE("serve_connection: a long stream of small writes to a peer that takes them runs no risk", "[server][timer]") {
  virtual_time time;
  auto const router = make_paced_router();
  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;
  std::size_t taken = 0;

  // Each write earns about 2 ms and takes 5: that would run out of a timeout in about 1000 of them.
  options.write_timeout = 3s;
  options.lingering_timeout = 0s;

  std::string const raw = "GET /events HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(slow_reader{std::move(server), 2000, taken, 5ms}, router, options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  REQUIRE(taken > std::size_t{2000} * 20);
}

TEST_CASE("serve_connection: 100 Continue gives a body all of the time again, at a rate too", "[server][timer]") {
  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000000;

  // More than a timeout in all: its head takes most of the first, and its body most of the second.
  auto const in_stages = [&](io::memory_stream client) -> task<std::string> {
    auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
      auto const head = "POST " + target +
                        " HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 4\r\nExpect: 100-continue\r\n";
      std::string_view const end = "\r\n";
      std::string_view const body = "ping";

      (void)co_await io::write(client, std::as_bytes(std::span{head}));
      co_await quoll::async::sleep_for(2s);
      (void)co_await io::write(client, std::as_bytes(std::span{end}));
      co_await quoll::async::sleep_for(2s);
      (void)co_await io::write(client, std::as_bytes(std::span{body}));
    }());

    co_return received;
  };

  auto const got = paced_session(time, options, in_stages);

  REQUIRE(got.received.starts_with("HTTP/1.1 100"));
  REQUIRE(got.received.find("HTTP/1.1 200") != std::string::npos);
}

TEST_CASE("serve_connection: a stall of the loop or of a peer is forgiven by the writes it takes at once",
          "[server][timer]") {
  virtual_time time;
  auto const router = make_paced_router();
  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;
  std::size_t taken = 0;

  // Each stall takes most of the timeout, and what comes between them earns far less than that.
  options.write_timeout = 3s;
  options.lingering_timeout = 0s;

  std::string const raw = "GET /events HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  auto const answer = [&]() -> task<int> {
    (void)co_await io::write(client, std::as_bytes(std::span{raw}));
    co_await quoll::server::serve_connection(slow_reader{std::move(server), 2000, taken, 1ms, 2500ms, 500}, router,
                                             options);
    co_return 1;
  };

  REQUIRE(quoll::test::run(time, answer()) == 1);
  REQUIRE(taken > std::size_t{2000} * 20);
}

TEST_CASE("serve_connection: the body of a request earns from its first byte, after another on a connection",
          "[server][timer]") {
  auto const target = GENERATE(std::string{"/echo"}, std::string{"/pieces"});
  virtual_time time;
  connection_options options;

  options.read_timeout = 3s;
  options.min_read_rate = 1000;

  auto const two = [&](io::memory_stream client) -> task<std::string> {
    auto const [received, sent] = co_await quoll::async::when_all(read_until_closed(client), [&]() -> task<> {
      auto const first = "POST " + target + " HTTP/1.1\r\nHost: h\r\nContent-Length: 3000\r\n\r\n" +
                         std::string(std::size_t{3000}, 'x');
      auto const second =
          "POST " + target + " HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 7000\r\n\r\n";
      std::string const part(std::size_t{3500}, 'x');

      (void)co_await io::write(client, std::as_bytes(std::span{first}));
      co_await quoll::async::sleep_for(1ms);
      (void)co_await io::write(client, std::as_bytes(std::span{second}));

      // Late for what the head left, and what it brings has to pay for the wait for the next one.
      for (auto index = 0; index < 2; ++index) {
        co_await quoll::async::sleep_for(2s);
        (void)co_await io::write(client, std::as_bytes(std::span{part}));
      }
    }());

    co_return received;
  };

  auto const got = paced_session(time, options, two);

  REQUIRE(got.received.starts_with("HTTP/1.1 200"));
  REQUIRE(got.received.find("HTTP/1.1 200", 1) != std::string::npos);
}

TEST_CASE("serve_connection: what a stop throws through a handler is no failure of the handler", "[server][timer]") {
  virtual_time time;
  http::router router;
  auto reported = 0;
  std::stop_source stop;

  router.get("/stopped", [&](http::request) -> task<http::response> {
    stop.request_stop();
    co_await quoll::async::current_stop_token();
    throw std::system_error{std::make_error_code(std::errc::operation_canceled)};
  });

  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++reported; };
  options.lingering_timeout = 0s;

  auto const peer = [](io::memory_stream stream) -> task<int> {
    std::string_view const raw = "GET /stopped HTTP/1.1\r\nHost: h\r\n\r\n";

    (void)co_await io::write(stream, std::as_bytes(std::span{raw}));
    co_return 1;
  };

  auto const serving = [&](io::memory_stream stream) -> task<> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stop.get_token()};
  };

  static_cast<void>(
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client)))));

  REQUIRE(reported == 0);
}
