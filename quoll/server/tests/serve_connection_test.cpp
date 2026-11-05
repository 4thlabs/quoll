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

#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
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

    co_return response;
  });
  // Catches what stops it, and reads again.
  router.post("/stubborn", [](http::body_reader body) -> task<http::response> {
    http::response response;

    for (auto attempt = 0; attempt < 2; ++attempt) {
      try {
        while (auto const piece = co_await body.read()) {
          response.body += *piece;
        }
      } catch (std::system_error const &thrown) {
        response.fields.add("X-Failed", thrown.code().message());
      }
    }

    co_return response;
  });
  // Catches what stops it, and answers something else.
  router.post("/catching", [](http::body_reader body) -> task<http::response> {
    http::response response;

    try {
      while (co_await body.read()) {
      }
    } catch (std::system_error const &) {
      response.body = "caught";
    }

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

TEST_CASE("serve_connection: a body in pieces past its limit", "[server]") {
  connection_options options;

  options.limits.max_streamed_body_size = 4;

  SECTION("announced: refused at its head") {
    auto const got =
        serve("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", {verb::post}, true, options);

    REQUIRE(got.responses.at(0).status == http::status::content_too_large);
    REQUIRE(got.closed);
  }

  SECTION("as it comes: the handler's response gives way") {
    auto const got =
        serve("POST /catching HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
              {verb::post}, true, options);

    REQUIRE(got.responses.at(0).status == http::status::content_too_large);
    REQUIRE(got.responses.at(0).body.empty());
    REQUIRE(got.closed);
  }
}

TEST_CASE("serve_connection: what stops the reading of a body is no failure of the handler", "[server]") {
  auto failures = 0;
  connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++failures; };
  options.limits.max_streamed_body_size = 4;

  SECTION("too large, through the handler") {
    auto const got =
        serve("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
              {verb::post}, true, options);

    REQUIRE(got.responses.at(0).status == http::status::content_too_large);
    REQUIRE(failures == 0);
  }

  SECTION("what is no HTTP") {
    options.limits.max_streamed_body_size = 1024;

    auto const got = serve("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", {verb::post},
                           true, options);

    REQUIRE(got.responses.at(0).status == http::status::bad_request);
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

TEST_CASE("serve_connection: a body in pieces has a read timeout per piece", "[server][timer]") {
  quoll::test::virtual_time time;
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  connection_options options;

  options.read_timeout = 3s;

  // Each piece comes in time, for longer than one timeout in all, and the last never does.
  auto const peer = [](io::memory_stream stream) -> task<http::response> {
    std::string const head = "POST /catching HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n";

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
  connection_options options;

  // In one read: a chunk that fits, and one past the limit.
  options.limits.max_streamed_body_size = 4;

  auto const got =
      serve("POST /stubborn HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nping\r\n1\r\nx\r\n0\r\n\r\n",
            {verb::post}, true, options);

  // The handler saw what came and failed twice, and the refusal is what goes out, in the place of what it made of it.
  REQUIRE(got.responses.at(0).status == http::status::content_too_large);
  REQUIRE(got.closed);
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
    co_return true;
  };

  auto const serving = [&](io::memory_stream stream) -> task<bool> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve_connection(std::move(stream), router, options),
                                                    stopping.get_token()};
    co_return true;
  };

  auto const outcome =
      quoll::test::run(time, quoll::async::when_all(serving(std::move(server)), peer(std::move(client))));

  REQUIRE(outcome);
  REQUIRE(failures == 0);
}
