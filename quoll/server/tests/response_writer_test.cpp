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
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/event_writer.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;

using async::task;
using quoll::test::read_until_closed;
using quoll::test::virtual_time;
using namespace std::chrono_literals;

// What a peer gets for the bytes `sent`, as they are.
std::string answer_to(http::server const &server, std::string const &sent) {
  virtual_time time;
  auto [client, ours] = io::make_memory_stream_pair();

  auto const peer = [&sent](io::memory_stream to) -> task<std::string> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    co_return co_await read_until_closed(to);
  };

  auto outcome =
      quoll::test::run(time, async::when_all(server.serve_connection(std::move(ours)), peer(std::move(client))));

  return std::move(std::get<1>(outcome.value()));
}

std::string get(std::string_view target) {
  return "GET " + std::string{target} + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
}

} // namespace

TEST_CASE("response_writer: a handler writes its body in pieces", "[server]") {
  http::router router;

  router.get("/rows/{name}", [](std::string_view name, http::response_writer out) -> task<> {
    co_await out.write(name);
    co_await async::sleep_for(1s);
    co_await out.write(" and more");
  });

  auto const answer = answer_to(http::server{router}, get("/rows/first"));

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("Transfer-Encoding: chunked") != std::string::npos);
  REQUIRE(answer.find("5\r\nfirst\r\n") != std::string::npos);
  REQUIRE(answer.find("9\r\n and more\r\n") != std::string::npos);
  REQUIRE(answer.ends_with("0\r\n\r\n"));
}

TEST_CASE("response_writer: its status and fields are the response's, added to what a middleware added", "[server]") {
  http::router router;
  std::string seen;

  router.use([&seen](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    // The handler has not run yet.
    seen = std::to_string(static_cast<int>(answer.status)) + ' ' +
           std::string{answer.fields.find("X-Handler").value_or("none")};
    answer.fields.set("X-Middleware", "added");
    answer.fields.set("X-Both", "middleware");
    co_return answer;
  });

  router.get("/made", [](http::response_writer out) -> task<> {
    // Waits before it writes, as a handler loading its data does: the router waits for it.
    co_await async::sleep_for(1s);
    out.status = http::status::created;
    out.fields.set("X-Handler", "set");
    out.fields.set("X-Both", "handler");
    co_await out.write("body");
    out.fields.set("X-Late", "too late");
  });

  auto const answer = answer_to(http::server{router}, get("/made"));

  REQUIRE(seen == "200 none");
  REQUIRE(answer.find("X-Both: handler") != std::string::npos);
  REQUIRE(answer.find("X-Both: middleware") == std::string::npos);
  REQUIRE(answer.starts_with("HTTP/1.1 201"));
  REQUIRE(answer.find("X-Handler: set") != std::string::npos);
  REQUIRE(answer.find("X-Middleware: added") != std::string::npos);
  REQUIRE(answer.find("X-Late") == std::string::npos);
  REQUIRE(answer.find("4\r\nbody\r\n") != std::string::npos);
}

TEST_CASE("response_writer: a Content-Length frames what it writes", "[server]") {
  http::router router;

  router.get("/sized", [](http::response_writer out) -> task<> {
    out.fields.set("Content-Length", "5");
    co_await out.write("he");
    co_await out.write("llo");
  });

  auto const answer = answer_to(http::server{router}, get("/sized"));

  REQUIRE(answer.find("Transfer-Encoding") == std::string::npos);
  REQUIRE(answer.ends_with("\r\n\r\nhello"));
}

TEST_CASE("response_writer: an empty write sends the head at once", "[server]") {
  http::router router;

  router.get("/announced", [](http::response_writer out) -> task<> {
    co_await out.write({});
    co_await out.write("data");
  });

  REQUIRE(answer_to(http::server{router}, get("/announced")).ends_with("4\r\ndata\r\n0\r\n\r\n"));
}

TEST_CASE("response_writer: a handler that writes nothing answers with an empty body", "[server]") {
  http::router router;

  router.get("/nothing", [](http::response_writer out) -> task<> {
    out.status = http::status::accepted;
    co_return;
  });

  auto const answer = answer_to(http::server{router}, get("/nothing"));

  REQUIRE(answer.starts_with("HTTP/1.1 202"));
  REQUIRE(answer.find("Content-Length: 0") != std::string::npos);
}

TEST_CASE("response_writer: what it throws before writing answers 500, and after cuts the connection", "[server]") {
  http::router router;
  int told = 0;

  router.get("/before", [](http::response_writer out) -> task<> {
    out.status = http::status::created;
    throw std::runtime_error{"on purpose"};
    co_return;
  });
  router.get("/after", [](http::response_writer out) -> task<> {
    co_await out.write("started");
    throw std::runtime_error{"on purpose"};
  });

  http::server const server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { ++told; }}}};

  REQUIRE(answer_to(server, get("/before")).starts_with("HTTP/1.1 500"));
  REQUIRE(told == 1);

  auto const cut = answer_to(server, get("/after"));

  REQUIRE(cut.starts_with("HTTP/1.1 200"));
  REQUIRE(cut.find("started") != std::string::npos);
  REQUIRE_FALSE(cut.ends_with("0\r\n\r\n"));
  REQUIRE(told == 2);
}

TEST_CASE("response_writer: a parameter that cannot be read answers 400, without running it", "[server]") {
  http::router router;
  bool ran = false;

  router.get("/n/{id}", [&ran](int, http::response_writer) -> task<> {
    ran = true;
    co_return;
  });

  REQUIRE(answer_to(http::server{router}, get("/n/not-a-number")).starts_with("HTTP/1.1 400"));
  REQUIRE_FALSE(ran);
}

TEST_CASE("response_writer: a route reads as it writes", "[server]") {
  http::router router;

  router.post("/echo", [](http::body_reader in, http::response_writer out) -> task<> {
    while (auto const piece = co_await in.read()) {
      co_await out.write(*piece);
    }
  });

  auto const answer = answer_to(
      http::server{router}, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("hello") != std::string::npos);
  REQUIRE(answer.ends_with("0\r\n\r\n"));
}

TEST_CASE("response_writer: when its body is not wanted, write ends the handler", "[server]") {
  http::router router;
  std::error_code ended_with;
  auto const writer = [&ended_with](http::response_writer out) -> task<> {
    out.fields.set("X-Handler", "ran");

    try {
      co_await out.write("never sent");
    } catch (std::system_error const &failure) {
      ended_with = failure.code();
      throw;
    }
  };

  router.get("/rows", writer);

  http::server const server{router};

  auto const answer = answer_to(server, "HEAD /rows HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("X-Handler: ran") != std::string::npos);
  REQUIRE(answer.find("never sent") == std::string::npos);

  REQUIRE(ended_with == std::errc::operation_canceled);
}

TEST_CASE("response_writer: to HEAD, a write that was refused stays refused", "[server]") {
  http::router router;
  int refused = 0;

  router.get("/rows", [&refused](http::response_writer out) -> task<> {
    for (auto attempt = 0; attempt < 3; ++attempt) {
      try {
        co_await out.write("sent to GET");
      } catch (std::system_error const &failure) {
        REQUIRE(failure.code() == std::errc::operation_canceled);
        ++refused;
      }
    }
  });

  auto const answer = answer_to(http::server{router}, "HEAD /rows HTTP/1.1\r\nHost: x\r\n\r\n"
                                                      "GET /rows HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  REQUIRE(refused == 3);
  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("HTTP/1.1 200", 1) != std::string::npos);
  REQUIRE(answer.find("sent to GET") != std::string::npos);
}

TEST_CASE("response_writer: a head that cannot leave answers 500, and the handler ends", "[server]") {
  http::router router;
  int told = 0;
  std::string caught;

  router.get("/bad", [&caught](http::response_writer out) -> task<> {
    out.fields.set("X-Bad", "a\nb");

    try {
      co_await out.write("never sent");
    } catch (std::exception const &failure) {
      caught = failure.what();
      throw;
    }
  });

  http::server const server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { ++told; }}}};

  REQUIRE(answer_to(server, get("/bad")).starts_with("HTTP/1.1 500"));
  REQUIRE_FALSE(caught.empty());
  REQUIRE(told == 1);
}

TEST_CASE("response_writer: a timeout around next bounds nothing of what it writes", "[server][timer]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    co_return co_await async::with_timeout(1s, next(std::move(request)));
  });
  router.get("/slow", [](http::response_writer out) -> task<> {
    co_await async::sleep_for(5s);
    co_await out.write("late");
  });

  auto const answer = answer_to(http::server{router}, get("/slow"));

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("late") != std::string::npos);
}

TEST_CASE("response_writer: a middleware replacing its answer leaves it unrun", "[server]") {
  http::router router;
  bool ran = false;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    (void)co_await next(std::move(request));

    http::response replaced;

    replaced.body = "replaced";
    co_return replaced;
  });
  router.get("/rows", [&ran](http::response_writer) -> task<> {
    ran = true;
    co_return;
  });

  auto const answer = answer_to(http::server{router}, get("/rows"));

  REQUIRE(answer.ends_with("replaced"));
  REQUIRE_FALSE(ran);
}

TEST_CASE("response_writer: what would mislead its peer answers as for any handler", "[server]") {
  http::router router;
  int told = 0;

  router.get("/early", [](http::response_writer out) -> task<> {
    out.status = http::status::continue_;
    co_await out.write("never sent");
  });
  router.get("/short", [](http::response_writer out) -> task<> {
    out.fields.set("Content-Length", "5");
    co_return;
  });

  http::server const server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { ++told; }}}};

  REQUIRE(answer_to(server, get("/early")).starts_with("HTTP/1.1 500"));
  REQUIRE(told == 1);
  REQUIRE(answer_to(server, get("/short")).starts_with("HTTP/1.1 500"));
  REQUIRE(told == 2);
}

TEST_CASE("response_writer: a stop request ends a handler waiting after its first write", "[server][timer]") {
  http::router router;
  std::error_code ended_with;
  std::chrono::nanoseconds stopped_at{};

  virtual_time time;

  router.get("/slow", [&](http::response_writer out) -> task<> {
    co_await out.write("started");

    try {
      co_await async::sleep_for(1h);
    } catch (std::system_error const &failure) {
      ended_with = failure.code();
      stopped_at = time.now();
      throw;
    }
  });

  http::server const server{router};
  auto [client, ours] = io::make_memory_stream_pair();
  std::stop_source stop;
  std::string received;
  auto const sent = get("/slow");

  auto const peer = [&](io::memory_stream to) -> task<> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    received = co_await read_until_closed(to);
  };
  auto const stopping = [&]() -> task<> {
    co_await async::sleep_for(1s);
    stop.request_stop();
  };
  auto const ignored = quoll::test::overloaded{[] {}, [](std::exception_ptr const &) {}};

  async::co_spawn(time.get_executor(), server.serve_connection(std::move(ours)), ignored, stop.get_token());
  async::co_spawn(time.get_executor(), peer(std::move(client)), ignored);
  async::co_spawn(time.get_executor(), stopping(), ignored);
  time.run();

  REQUIRE(ended_with == std::errc::operation_canceled);
  REQUIRE(stopped_at == 1s);
  REQUIRE(received.find("started") != std::string::npos);
}

TEST_CASE("response_writer: through local_transport, as through a connection", "[server]") {
  http::router router;

  router.get("/rows", [](http::response_writer out) -> task<> {
    out.status = http::status::created;
    co_await out.write("one ");
    co_await out.write("two");
  });

  http::local_transport const transport{router};
  http::request request;

  request.target = "/rows";

  async::run_loop loop;
  auto const response = quoll::test::run(loop, transport.send(request));

  REQUIRE(response.value().status == http::status::created);
  REQUIRE(response.value().body == "one two");
}

TEST_CASE("response_writer: a body refused by the server is answered as such, not with an empty 200", "[server]") {
  http::router router;

  router.post("/echo", [](http::body_reader in, http::response_writer out) -> task<> {
    while (auto const piece = co_await in.read()) {
      co_await out.write(*piece);
    }
  });

  http::server const server{router, {.connection = {.limits = {.max_body_size = 4, .max_streamed_body_size = 4}}}};
  auto const answer = answer_to(server, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                                        "a\r\n0123456789\r\n0\r\n\r\n");

  REQUIRE(answer.starts_with("HTTP/1.1 413"));
}

TEST_CASE("response_writer: a stop request before its first write answers nothing", "[server][timer]") {
  http::router router;
  virtual_time time;

  router.get("/slow", [](http::response_writer out) -> task<> {
    co_await async::sleep_for(1h);
    co_await out.write("late");
  });

  http::server const server{router};
  auto [client, ours] = io::make_memory_stream_pair();
  std::stop_source stop;
  std::string received;
  auto const sent = get("/slow");

  auto const peer = [&](io::memory_stream to) -> task<> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    received = co_await read_until_closed(to);
  };
  auto const stopping = [&]() -> task<> {
    co_await async::sleep_for(1s);
    stop.request_stop();
  };
  auto const ignored = quoll::test::overloaded{[] {}, [](std::exception_ptr const &) {}};

  async::co_spawn(time.get_executor(), server.serve_connection(std::move(ours)), ignored, stop.get_token());
  async::co_spawn(time.get_executor(), peer(std::move(client)), ignored);
  async::co_spawn(time.get_executor(), stopping(), ignored);
  time.run();

  REQUIRE(received.empty());
}

TEST_CASE("response_writer: taken by reference, it goes with its handler", "[server]") {
  http::router router;

  router.get("/throws", [](http::response_writer &out) -> task<> {
    out.status = http::status::created;
    throw std::runtime_error{"on purpose"};
    co_return;
  });
  router.get("/nothing", [](http::response_writer &out) -> task<> {
    out.status = http::status::accepted;
    co_return;
  });
  router.get("/moved", [](http::response_writer &&out) -> task<> {
    out.status = http::status::created;
    co_await out.write("body");
  });

  http::server const server{router};

  REQUIRE(answer_to(server, get("/throws")).starts_with("HTTP/1.1 500"));
  REQUIRE(answer_to(server, get("/nothing")).starts_with("HTTP/1.1 202"));
  REQUIRE(answer_to(server, get("/moved")).starts_with("HTTP/1.1 201"));

  http::local_transport const transport{router};
  http::request request;

  request.target = "/nothing";

  async::run_loop loop;

  REQUIRE(quoll::test::run(loop, transport.send(request)).value().status == http::status::accepted);
}

TEST_CASE("response_writer: a body read whole by its first write leaves the connection open", "[server]") {
  http::router router;

  router.post("/echo", [](http::body_reader in, http::response_writer out) -> task<> {
    std::string body;

    while (auto const piece = co_await in.read()) {
      body += *piece;
    }

    co_await out.write(body);
  });
  router.get("/next", [](http::request) {
    http::response response;

    response.body = "next";
    return response;
  });

  auto const answer = answer_to(http::server{router}, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                                                      "GET /next HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  REQUIRE(answer.find("hello") != std::string::npos);
  REQUIRE(answer.ends_with("next"));
}

TEST_CASE("response_writer: to HEAD, what asks for a body gets 100 Continue first, as it may read before it writes",
          "[server]") {
  http::router router;
  auto read = false;

  router.get("/rows", [&read](http::body_reader in, http::response_writer out) -> task<> {
    read = (co_await in.read()).has_value();
    co_await out.write("never sent");
  });

  auto const body_after_continue = [&router]() -> std::string {
    virtual_time time;
    auto [client, ours] = io::make_memory_stream_pair();
    std::string const head =
        "HEAD /rows HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";

    auto const peer = [&head](io::memory_stream to) -> task<std::string> {
      (void)co_await io::write(to, std::as_bytes(std::span{head}));

      // Sends its body once told to.
      std::array<char, 64> interim;
      auto const got = co_await to.read_some(std::as_writable_bytes(std::span{interim}));
      std::string const body = "hello";

      (void)co_await io::write(to, std::as_bytes(std::span{body}));
      co_return std::string(interim.data(), got.transferred) + co_await read_until_closed(to);
    };

    auto outcome = quoll::test::run(
        time, async::when_all(http::server{router}.serve_connection(std::move(ours)), peer(std::move(client))));

    return std::move(std::get<1>(outcome.value()));
  }();

  REQUIRE(body_after_continue.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200"));
  REQUIRE_FALSE(body_after_continue.contains("never sent"));
  REQUIRE(read);
}

TEST_CASE("response_writer: a drain that began while it waited closes the connection", "[server][timer]") {
  http::router router;

  router.get("/slow", [](http::response_writer out) -> task<> {
    co_await async::sleep_for(5s);
    co_await out.write("late");
  });

  std::stop_source drain;
  http::server const server{router, {.connection = {.drain = drain.get_token()}}};
  virtual_time time;
  auto [client, ours] = io::make_memory_stream_pair();
  std::string received;
  std::string const sent = "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n";

  auto const peer = [&](io::memory_stream to) -> task<> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    received = co_await read_until_closed(to);
  };
  auto const draining = [&]() -> task<> {
    co_await async::sleep_for(1s);
    drain.request_stop();
  };
  auto const ignored = quoll::test::overloaded{[] {}, [](std::exception_ptr const &) {}};

  async::co_spawn(time.get_executor(), server.serve_connection(std::move(ours)), ignored);
  async::co_spawn(time.get_executor(), peer(std::move(client)), ignored);
  async::co_spawn(time.get_executor(), draining(), ignored);
  time.run();

  REQUIRE(received.find("Connection: close") != std::string::npos);
  REQUIRE(received.find("late") != std::string::npos);
}

namespace {

// Passes a handler's head on, as a middleware wrapping its writer has to.
struct wrapping_sink {
  http::body_stream out;
  std::size_t *written;

  void head(http::status status, http::fields fields) { out.head(status, std::move(fields)); }

  [[nodiscard]] task<> write(std::string_view piece) {
    *written += piece.size();
    co_await out.write(piece);
  }
};

// Reads what its peer sends, and fails every write.
class failing_writes {
public:
  explicit failing_writes(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    co_return io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
  }

  [[nodiscard]] task<io::io_result> shutdown_send() { co_return io::io_result{}; }

private:
  io::memory_stream inner_;
};

} // namespace

TEST_CASE("response_writer: a middleware wrapping its writer passes its head on", "[server]") {
  http::router router;
  std::size_t written = 0;

  router.use([&written](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.late_head_writer = [&written, inner = std::move(answer.late_head_writer)](http::body_stream out) -> task<> {
      wrapping_sink sink{out, &written};

      co_await inner(http::body_stream{sink});
    };
    co_return answer;
  });
  router.get("/rows", [](http::response_writer out) -> task<> {
    out.status = http::status::created;
    co_await out.write("body");
  });

  auto const answer = answer_to(http::server{router}, get("/rows"));

  REQUIRE(answer.starts_with("HTTP/1.1 201"));
  REQUIRE(written == 4);
}

TEST_CASE("response_writer: what a handler asks of the connection stands, whatever replaces its answer", "[server]") {
  http::router router;

  router.get("/short", [](http::response_writer out) -> task<> {
    out.fields.set("Connection", "close");
    out.fields.set("Content-Length", "5");
    co_return;
  });
  router.get("/throws", [](http::response_writer out) -> task<> {
    out.fields.set("Connection", "close");
    throw std::runtime_error{"on purpose"};
    co_return;
  });
  router.get("/next", [](http::request) { return http::response{}; });

  http::server const server{router};
  std::string const next = "GET /next HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  auto const keeping_on = [](std::string_view target) {
    return "GET " + std::string{target} + " HTTP/1.1\r\nHost: x\r\n\r\n";
  };

  for (auto const *const target : {"/short", "/throws"}) {
    auto const answer = answer_to(server, keeping_on(target) + next);

    REQUIRE(answer.starts_with("HTTP/1.1 500"));
    REQUIRE(answer.find("Connection: close") != std::string::npos);
    REQUIRE(answer.find("HTTP/1.1 200") == std::string::npos);
  }
}

TEST_CASE("response_writer: a handler catching what its head threw can answer otherwise", "[server]") {
  http::router router;

  router.get("/fallback", [](http::response_writer out) -> task<> {
    out.status = http::status::no_content;

    try {
      // A 204 has no body: this cannot go.
      co_await out.write("x");
    } catch (std::exception const &) {
      // It answers with what it set.
    }
  });

  REQUIRE(answer_to(http::server{router}, get("/fallback")).starts_with("HTTP/1.1 204"));
}

TEST_CASE("response_writer: write on one moved from throws", "[server]") {
  http::router router;
  bool refused = false;

  router.get("/moved", [&refused](http::response_writer out) -> task<> {
    auto const taken = std::move(out);

    try {
      // What is tested, which tidy and its analyzer both report.
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      (void)out.write("x");
    } catch (std::logic_error const &) {
      refused = true;
    }

    co_return;
  });

  (void)answer_to(http::server{router}, get("/moved"));

  REQUIRE(refused);
}

TEST_CASE("response_writer: Set-Cookie and Vary add up, where other fields replace", "[server]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.fields.add("Set-Cookie", "session=1");
    answer.fields.add("Vary", "Origin");
    co_return answer;
  });
  router.get("/rows", [](http::response_writer out) -> task<> {
    out.fields.add("Set-Cookie", "theme=dark");
    out.fields.add("Vary", "Accept-Encoding");
    co_await out.write("body");
  });

  auto const answer = answer_to(http::server{router}, get("/rows"));

  REQUIRE(answer.find("session=1") != std::string::npos);
  REQUIRE(answer.find("theme=dark") != std::string::npos);
  REQUIRE(answer.find("Origin") != std::string::npos);
  REQUIRE(answer.find("Accept-Encoding") != std::string::npos);
}

TEST_CASE("response_writer: to HTTP/1.0, the body ends with the connection", "[server]") {
  http::router router;

  router.get("/rows", [](http::response_writer out) -> task<> { co_await out.write("body"); });

  auto const answer = answer_to(http::server{router}, "GET /rows HTTP/1.0\r\n\r\n");

  REQUIRE(answer.find("Transfer-Encoding") == std::string::npos);
  REQUIRE(answer.find("Connection: close") != std::string::npos);
  REQUIRE(answer.ends_with("\r\n\r\nbody"));
}

TEST_CASE("response_writer: to HEAD, a head that fails to leave ends the connection", "[server]") {
  http::router router;
  int ran = 0;

  router.get("/rows", [&ran](http::response_writer out) -> task<> {
    ++ran;
    co_await out.write("never sent");
  });

  http::server const server{router};
  virtual_time time;
  auto [client, ours] = io::make_memory_stream_pair();
  std::string const sent = "HEAD /rows HTTP/1.1\r\nHost: x\r\n\r\nHEAD /rows HTTP/1.1\r\nHost: x\r\n\r\n";

  auto const peer = [&sent](io::memory_stream to) -> task<int> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    co_return 1;
  };

  (void)quoll::test::run(
      time, async::when_all(server.serve_connection(failing_writes{std::move(ours)}), peer(std::move(client))));

  REQUIRE(ran == 1);
}

TEST_CASE("response_writer: to HEAD through local_transport, a stop is no answer", "[server][timer]") {
  http::router router;

  router.get("/slow", [](http::response_writer out) -> task<> {
    co_await async::sleep_for(1h);
    co_await out.write("late");
  });

  http::local_transport const transport{router};
  http::request request;

  request.method = http::verb::head;
  request.target = "/slow";

  virtual_time time;
  std::error_code ended_with;

  auto const asking = [&]() -> task<int> {
    try {
      (void)co_await async::with_timeout(1s, transport.send(request));
    } catch (std::system_error const &failure) {
      ended_with = failure.code();
    }

    co_return 1;
  };

  (void)quoll::test::run(time, asking());

  REQUIRE(ended_with == std::errc::timed_out);
}

TEST_CASE("response_writer: a route's body stays until it has read it, past dispatch()", "[server]") {
  http::router router;
  std::string read_back;

  router.post("/echo", [&read_back](http::body_reader in, http::response_writer) -> task<> {
    while (auto const piece = co_await in.read()) {
      read_back += *piece;
    }
  });

  http::request request;

  request.method = http::verb::post;
  request.target = "/echo";
  request.body = std::string(100, 'y');

  struct nowhere {
    [[nodiscard]] task<> write(std::string_view) const { co_return; }
  };

  async::run_loop loop;
  auto response = quoll::test::run(loop, router.dispatch(std::move(request)));
  nowhere sink;
  auto const writer = response.value().late_head_writer;

  (void)quoll::test::run(loop, [&]() -> task<int> {
    co_await writer(http::body_stream{sink});
    co_return 1;
  }());

  REQUIRE(read_back == std::string(100, 'y'));
}

TEST_CASE("response_writer: to HEAD, a Transfer-Encoding announces no length", "[server]") {
  http::router router;

  router.get("/piped", [](http::response_writer out) -> task<> {
    out.fields.set("Transfer-Encoding", "chunked");
    co_await out.write("hello world");
  });

  auto const answer = answer_to(http::server{router}, "HEAD /piped HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE_FALSE(answer.contains("Content-Length"));
  REQUIRE_FALSE(answer.contains("Transfer-Encoding"));
}

TEST_CASE("response_writer: a body beside what writes one answers 500, HEAD included, and nothing runs", "[server]") {
  http::router router;
  auto ran = 0;
  auto told = 0;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.body = "from-middleware";
    co_return answer;
  });
  router.get("/handler", [&ran](http::response_writer out) -> task<> {
    ++ran;
    co_await out.write("from-handler");
  });
  router.get("/writer", [&ran](http::request) {
    http::response answer;

    answer.body_writer = [&ran](http::body_stream out) -> task<> {
      ++ran;
      co_await out.write("from-writer");
    };
    return answer;
  });

  auto const target = GENERATE(as<std::string>{}, "/handler", "/writer");
  auto const method = GENERATE(http::verb::get, http::verb::head);
  http::server const server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { ++told; }}}};
  auto const answer = answer_to(server, std::string{method == http::verb::get ? "GET " : "HEAD "} + target +
                                            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  REQUIRE(answer.starts_with("HTTP/1.1 500"));
  REQUIRE_FALSE(answer.contains("from-"));
  REQUIRE(told == 1);

  // As it does in the same program.
  http::local_transport const transport{router};
  http::request request;

  request.method = method;
  request.target = target;

  virtual_time time;
  auto const local = quoll::test::run(time, transport.send(request));

  REQUIRE(local.value().status == http::status::internal_server_error);
  REQUIRE_FALSE(local.value().body.contains("from-"));
  REQUIRE(ran == 0);
}

TEST_CASE("response_writer: through local_transport, a write after a stop throws", "[server][timer]") {
  http::router router;
  auto written = 0;

  router.get("/handler", [&written](http::response_writer out) -> task<> {
    try {
      co_await async::sleep_for(1h);
    } catch (std::system_error const &) {
      // Takes the stop for the end of its wait, and goes on.
    }

    co_await out.write("late");
    ++written;
  });
  router.get("/writer", [&written](http::request) {
    http::response answer;

    answer.body_writer = [&written](http::body_stream out) -> task<> {
      try {
        co_await async::sleep_for(1h);
      } catch (std::system_error const &) {
        // The same.
      }

      co_await out.write("late");
      ++written;
    };
    return answer;
  });

  http::local_transport const transport{router};
  auto const target = GENERATE(as<std::string>{}, "/handler", "/writer");
  http::request request;

  request.method = http::verb::get;
  request.target = target;

  virtual_time time;
  auto stopped = false;

  auto const asking = [&]() -> task<int> {
    try {
      (void)co_await async::with_timeout(1s, transport.send(request));
    } catch (std::system_error const &) {
      stopped = true;
    }

    co_return 1;
  };

  (void)quoll::test::run(time, asking());

  REQUIRE(stopped);
  REQUIRE(written == 0);
}

TEST_CASE("response_writer: a body_writer a middleware sets replaces the handler, HEAD included", "[server]") {
  http::router router;
  auto handler_ran = 0;
  auto writer_ran = 0;

  router.use([&writer_ran](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.body_writer = [&writer_ran](http::body_stream out) -> task<> {
      ++writer_ran;
      co_await out.write("cached");
    };
    co_return answer;
  });
  router.get("/rows", [&handler_ran](http::response_writer out) -> task<> {
    ++handler_ran;
    co_await out.write("from-handler");
  });

  http::server const server{router};

  REQUIRE(answer_to(server, "HEAD /rows HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").starts_with("HTTP/1.1 200"));
  // As http::body_writer says: not for HEAD.
  REQUIRE(writer_ran == 0);

  auto const answer = answer_to(server, get("/rows"));

  REQUIRE(answer.contains("cached"));
  REQUIRE_FALSE(answer.contains("from-handler"));
  REQUIRE(writer_ran == 1);
  REQUIRE(handler_ran == 0);
}

TEST_CASE("response_writer: its handler runs once, whatever holds a copy of it", "[server]") {
  http::router router;
  std::vector<std::string> names;

  router.get("/rows/{name}", [&names](std::string name, http::response_writer out) -> task<> {
    names.push_back(std::move(name));
    co_await out.write("row");
  });

  struct nowhere {
    [[nodiscard]] task<> write(std::string_view) const { co_return; }
  };

  http::request request;

  request.method = http::verb::get;
  request.target = "/rows/a-name-long-enough-for-the-heap";

  async::run_loop loop;
  auto response = quoll::test::run(loop, router.dispatch(std::move(request)));
  nowhere sink;
  auto const writer = response.value().late_head_writer;
  auto const again = writer;
  auto refused = false;

  (void)quoll::test::run(loop, [&]() -> task<int> {
    co_await writer(http::body_stream{sink});

    try {
      co_await again(http::body_stream{sink});
    } catch (std::logic_error const &) {
      refused = true;
    }

    co_return 1;
  }());

  REQUIRE(refused);
  REQUIRE(names == std::vector<std::string>{"a-name-long-enough-for-the-heap"});
}

TEST_CASE("server: HTTP/1.0 gets no chunks, from what writes nothing or a whole body", "[server]") {
  http::router router;

  router.get("/nothing", [](http::response_writer out) -> task<> {
    out.fields.set("Transfer-Encoding", "chunked");
    co_return;
  });
  router.get("/whole", [] {
    http::response answer;

    answer.fields.set("Transfer-Encoding", "chunked");
    answer.body = "whole";
    return answer;
  });

  http::server const server{router};
  auto const nothing = answer_to(server, "GET /nothing HTTP/1.0\r\nHost: x\r\n\r\n");

  REQUIRE(nothing.starts_with("HTTP/1.1 200"));
  REQUIRE_FALSE(nothing.contains("Transfer-Encoding"));
  // Its head alone, no terminating chunk.
  REQUIRE(nothing.ends_with("Content-Length: 0\r\n\r\n"));

  auto const whole = answer_to(server, "GET /whole HTTP/1.0\r\nHost: x\r\n\r\n");

  REQUIRE_FALSE(whole.contains("Transfer-Encoding"));
  REQUIRE(whole.contains("Content-Length: 5"));
  REQUIRE(whole.ends_with("\r\n\r\nwhole"));
}

TEST_CASE("response_writer: a handler taking the request as well", "[server]") {
  http::router router;

  router.get("/by-value/{name}", [](http::request request, std::string name, http::response_writer out) -> task<> {
    out.fields.set("X-Target", request.target);
    co_await out.write(name);
  });
  router.get("/by-reference", [](http::request const &request, http::response_writer out) -> task<> {
    co_await out.write(request.target);
  });

  http::server const server{router};
  auto const by_value = answer_to(server, get("/by-value/a"));

  REQUIRE(by_value.contains("X-Target: /by-value/a"));
  REQUIRE(by_value.ends_with("1\r\na\r\n0\r\n\r\n"));
  REQUIRE(answer_to(server, get("/by-reference")).contains("/by-reference"));
}

TEST_CASE("response_writer: to HEAD through local_transport, its first write sends its head alone", "[server]") {
  http::router router;
  auto went_on = false;

  router.get("/rows", [&went_on](http::response_writer out) -> task<> {
    out.status = http::status::created;
    out.fields.set("X-Handler", "set");
    co_await out.write("row");
    went_on = true;
  });

  http::local_transport const transport{router};
  http::request request;

  request.method = http::verb::head;
  request.target = "/rows";

  virtual_time time;
  auto const answer = quoll::test::run(time, transport.send(request));

  REQUIRE(answer.value().status == http::status::created);
  REQUIRE(answer.value().fields.find("X-Handler") == "set");
  REQUIRE(answer.value().body.empty());
  REQUIRE_FALSE(went_on);
}

TEST_CASE("response_writer: writes made before they are awaited send one head", "[server]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.late_head_writer = [](http::body_stream out) -> task<> {
      auto first = out.write("one");
      auto second = out.write("two");

      co_await std::move(first);
      co_await std::move(second);
    };
    co_return answer;
  });
  router.get("/rows", [](http::response_writer) -> task<> { co_return; });

  auto const answer = answer_to(http::server{router}, get("/rows"));
  auto const first_head = answer.find("HTTP/1.1 200");

  REQUIRE(first_head == 0);
  REQUIRE(answer.find("HTTP/1.1", first_head + 1) == std::string::npos);
  REQUIRE(answer.ends_with("3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n"));
}

TEST_CASE("server: a refusal already decided invites no body", "[server]") {
  http::router router;
  auto ran = false;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.status = http::status::unauthorized;
    answer.body_writer = [](http::body_stream out) -> task<> { co_await out.write("no"); };
    answer.late_head_writer = nullptr;
    co_return answer;
  });
  router.post("/upload", [&ran](http::body_reader) {
    ran = true;
    return http::response{};
  });

  auto const answer = answer_to(http::server{router}, "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                                                      "Expect: 100-continue\r\nConnection: close\r\n\r\n");

  REQUIRE(ran);
  REQUIRE(answer.starts_with("HTTP/1.1 401"));
  REQUIRE_FALSE(answer.contains("100 Continue"));
}

TEST_CASE("server: to HTTP/1.0, another coding than chunks answers 500 rather than pass for the content", "[server]") {
  http::router router;

  router.get("/whole", [] {
    http::response answer;

    answer.fields.set("Transfer-Encoding", "gzip, chunked");
    answer.body = "gzipped";
    return answer;
  });
  router.get("/two-lines", [] {
    http::response answer;

    answer.fields.add("Transfer-Encoding", "chunked");
    answer.fields.add("Transfer-Encoding", "gzip");
    answer.body = "gzipped";
    return answer;
  });
  router.get("/written", [](http::response_writer out) -> task<> {
    out.fields.set("Transfer-Encoding", "gzip, chunked");
    co_await out.write("gzipped");
  });

  http::server const server{router};
  auto const target = GENERATE(as<std::string>{}, "/whole", "/written", "/two-lines");
  auto const method = GENERATE(as<std::string>{}, "GET ", "HEAD ");
  auto const answer = answer_to(server, method + target + " HTTP/1.0\r\nHost: x\r\n\r\n");

  REQUIRE(answer.contains(" 500 "));
  REQUIRE_FALSE(answer.contains("gzipped"));
}

TEST_CASE("response_writer: through local_transport, a write that failed in a task of its own is no whole body",
          "[server][timer]") {
  http::router router;

  // Stopped by the timeout around it, not by the transport's token, and what it threw goes no further.
  auto const writing_late = [](http::body_stream out) -> task<> {
    co_await out.write("prefix");

    auto const late = [&out]() -> task<> {
      try {
        co_await async::sleep_for(1h);
      } catch (std::system_error const &) {
        // Goes on.
      }

      co_await out.write("suffix");
    };

    try {
      co_await async::with_timeout(1s, late());
    } catch (std::system_error const &) {
      // Makes nothing of it.
    }
  };

  router.get("/writer", [writing_late](http::request) {
    http::response answer;

    answer.body_writer = writing_late;
    return answer;
  });
  router.use([writing_late](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    if (answer.late_head_writer) {
      answer.late_head_writer = writing_late;
    }

    co_return answer;
  });
  router.get("/handler", [](http::response_writer) -> task<> { co_return; });

  http::local_transport const transport{router};
  auto const target = GENERATE(as<std::string>{}, "/writer", "/handler");
  http::request request;

  request.method = http::verb::get;
  request.target = target;

  virtual_time time;
  auto cut = false;

  auto const asking = [&]() -> task<int> {
    try {
      (void)co_await transport.send(request);
    } catch (std::system_error const &) {
      cut = true;
    }

    co_return 1;
  };

  (void)quoll::test::run(time, asking());

  REQUIRE(cut);
}

TEST_CASE("server: to HTTP/1.0, spaces around chunked change nothing", "[server]") {
  http::router router;

  router.get("/written", [](http::response_writer out) -> task<> {
    out.fields.set("Transfer-Encoding", " chunked ");
    co_await out.write("plain");
  });

  auto const answer = answer_to(http::server{router}, "GET /written HTTP/1.0\r\nHost: x\r\n\r\n");

  REQUIRE(answer.contains(" 200 "));
  REQUIRE_FALSE(answer.contains("Transfer-Encoding"));
  REQUIRE(answer.ends_with("\r\n\r\nplain"));
}

namespace {

std::string const asks_for_continue =
    " HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: "
    "close\r\n\r\n";

} // namespace

TEST_CASE("server: a writer reading what its refusal did not invite fails at once, rather than wait", "[server]") {
  http::router router;

  router.post("/refuse", [](http::body_reader body) {
    http::response answer;

    answer.status = http::status::bad_request;
    answer.body_writer = [body](http::body_stream out) mutable -> task<> {
      auto refused = false;

      try {
        (void)co_await body.read();
      } catch (std::system_error const &) {
        refused = true;
      }

      co_await out.write(refused ? "not read" : "read");
    };
    return answer;
  });

  auto const answer = answer_to(http::server{router}, "POST /refuse" + asks_for_continue);

  REQUIRE(answer.starts_with("HTTP/1.1 400"));
  REQUIRE(answer.contains("not read"));
  REQUIRE_FALSE(answer.contains("100 Continue"));
}

TEST_CASE("response_writer: a refusal a middleware set invites no body", "[server]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.status = http::status::unauthorized;
    co_return answer;
  });
  router.post("/upload", [](http::body_reader, http::response_writer out) -> task<> { co_await out.write("no"); });

  auto const answer = answer_to(http::server{router}, "POST /upload" + asks_for_continue);

  REQUIRE(answer.starts_with("HTTP/1.1 401"));
  REQUIRE_FALSE(answer.contains("100 Continue"));
}

TEST_CASE("server: to HEAD, a body_writer that does not run invites no body", "[server]") {
  http::router router;

  router.get("/rows", [](http::body_reader) {
    http::response answer;

    answer.body_writer = [](http::body_stream out) -> task<> { co_await out.write("never"); };
    return answer;
  });

  auto const answer = answer_to(http::server{router}, "HEAD /rows" + asks_for_continue);

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE_FALSE(answer.contains("100 Continue"));
}

TEST_CASE("server: a 100 Continue that cannot leave runs nothing", "[server]") {
  http::router router;
  auto ran = 0;

  router.post("/rows", [&ran](http::body_reader, http::response_writer out) -> task<> {
    ++ran;
    co_await out.write("never");
  });

  http::server const server{router};
  virtual_time time;
  auto [client, ours] = io::make_memory_stream_pair();

  auto const peer = [](io::memory_stream to) -> task<int> {
    auto const sent = "POST /rows" + asks_for_continue;

    (void)co_await io::write(to, std::as_bytes(std::span{sent}));
    (void)co_await read_until_closed(to);
    co_return 1;
  };

  (void)quoll::test::run(
      time, async::when_all(server.serve_connection(failing_writes{std::move(ours)}), peer(std::move(client))));

  REQUIRE(ran == 0);
}

TEST_CASE("response_writer: a reason a middleware set goes with its status", "[server]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.reason = "Everything Fine";
    co_return answer;
  });
  router.get("/rows", [](http::response_writer out) -> task<> {
    out.status = http::status::not_found;
    co_await out.write("gone");
  });

  REQUIRE(answer_to(http::server{router}, get("/rows")).starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE("server: HTTP/1.0 kept open says so", "[server]") {
  http::router router;

  router.get("/whole", [](http::request) {
    http::response answer;

    answer.body = "whole";
    return answer;
  });
  router.get("/written", [](http::response_writer out) -> task<> {
    out.fields.set("Content-Length", "7");
    co_await out.write("written");
  });

  http::server const server{router};
  auto const target = GENERATE(as<std::string>{}, "/whole", "/written");
  auto const kept = answer_to(server, "GET " + target + " HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");

  REQUIRE(kept.contains("Connection: keep-alive"));
  // Unless asked, it closes, as HTTP/1.0 does.
  REQUIRE_FALSE(answer_to(server, "GET " + target + " HTTP/1.0\r\n\r\n").contains("keep-alive"));
}

TEST_CASE("server: HTTP/1.0 kept open says so of what answers in its place, and of what else it names", "[server]") {
  http::router router;

  router.get("/throws", [](http::request) {
    http::response answer;

    answer.body_writer = [](http::body_stream) -> task<> { throw std::runtime_error{"before anything left"}; };
    answer.fields.set("Content-Length", "1");
    return answer;
  });
  router.get("/invalid", [](http::request) {
    http::response answer;

    answer.fields.add("Not A Token", "x");
    return answer;
  });
  router.get("/names-another", [](http::request) {
    http::response answer;

    answer.fields.set("Connection", "X-Internal");
    answer.fields.set("X-Internal", "1");
    return answer;
  });

  http::server const server{router};
  auto const target = GENERATE(as<std::string>{}, "/throws", "/invalid");
  auto const replaced = answer_to(server, "GET " + target + " HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");

  REQUIRE(replaced.contains(" 500 "));
  REQUIRE(replaced.contains("Connection: keep-alive"));

  auto const both = answer_to(server, "GET /names-another HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");

  REQUIRE(both.contains("Connection: X-Internal"));
  REQUIRE(both.contains("Connection: keep-alive"));
}

TEST_CASE("server: what cannot be written is told, as what a handler throws", "[server]") {
  http::router router;
  auto told = 0;

  router.get("/whole", [](http::request) {
    http::response answer;

    answer.fields.add("Not A Token", "x");
    return answer;
  });
  router.get("/in-pieces", [](http::request) {
    http::response answer;

    answer.fields.add("Not A Token", "x");
    answer.body_writer = [](http::body_stream) -> task<> { co_return; };
    return answer;
  });

  http::server const server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { ++told; }}}};
  auto const target = GENERATE(as<std::string>{}, "/whole", "/in-pieces");

  REQUIRE(answer_to(server, get(target)).starts_with("HTTP/1.1 500"));
  REQUIRE(told == 1);
}

TEST_CASE("response_writer: a 2xx given with its first write to CONNECT is no tunnel", "[server]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.status = http::status::forbidden;
    answer.late_head_writer = [](http::body_stream out) -> task<> {
      out.head(http::status::created, {});
      co_await out.write("tunnel");
    };
    co_return answer;
  });

  auto const answer = answer_to(http::server{router}, "CONNECT x:1 HTTP/1.1\r\nHost: x:1\r\n\r\n");

  REQUIRE_FALSE(answer.contains(" 201 "));
  REQUIRE(answer.contains(" 501 "));
}

TEST_CASE("event_writer: a text/event-stream, its head at once, an event a piece", "[server][sse]") {
  http::router router;

  router.get("/feed", [](http::response_writer out) -> task<> {
    out.fields.set("X-Feed", "yes");

    http::event_writer events{std::move(out)};

    co_await events.open();
    co_await async::sleep_for(1s);
    co_await events.retry(2s);
    co_await events.send({.type = "add", .data = "a\nb", .id = "1"});
    co_await events.comment("ping");
    co_await events.send({.data = "c"});
  });

  auto const answer = answer_to(http::server{router}, get("/feed"));

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("Content-Type: text/event-stream\r\n") != std::string::npos);
  REQUIRE(answer.find("X-Accel-Buffering: no\r\n") != std::string::npos);
  REQUIRE(answer.find("X-Feed: yes\r\n") != std::string::npos);
  REQUIRE(answer.find("Transfer-Encoding: chunked") != std::string::npos);
  // A chunk each, as it was sent.
  REQUIRE(answer.find("\r\nretry: 2000\n\r\n") != std::string::npos);
  REQUIRE(answer.find("\r\nevent: add\nid: 1\ndata: a\ndata: b\n\n\r\n") != std::string::npos);
  REQUIRE(answer.find("\r\n: ping\n\r\n") != std::string::npos);
  REQUIRE(answer.find("\r\ndata: c\n\n\r\n") != std::string::npos);
  REQUIRE(answer.ends_with("0\r\n\r\n"));
}

TEST_CASE("event_writer: open() sends the head before any event", "[server][sse]") {
  http::router router;

  router.get("/feed", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.open();
    co_await async::sleep_for(1h);
    co_await events.send({.data = "late"});
  });

  virtual_time time;
  auto [client, ours] = io::make_memory_stream_pair();
  std::optional<std::chrono::nanoseconds> head_at;

  // Reads up to the end of the head, notes when, then the rest.
  auto const peer = [&time, &head_at](io::memory_stream to) -> task<std::string> {
    std::string const request = get("/feed");
    std::string seen;
    std::array<char, 256> chunk{};

    (void)co_await io::write(to, std::as_bytes(std::span{request}));

    while (!seen.contains("\r\n\r\n")) {
      auto const read = co_await to.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error || read.transferred == 0) {
        co_return seen;
      }

      seen.append(chunk.data(), read.transferred);
    }

    head_at = time.now();
    co_return seen + co_await read_until_closed(to);
  };

  http::server const server{router};
  auto outcome =
      quoll::test::run(time, async::when_all(server.serve_connection(std::move(ours)), peer(std::move(client))));

  REQUIRE(head_at == 0s);
  REQUIRE(time.now() >= 1h);
  REQUIRE(std::get<1>(outcome.value()).find("data: late") != std::string::npos);
}

TEST_CASE("event_writer: a response_writer whose head has left, or moved from, is refused", "[server][sse]") {
  http::router router;
  int refused = 0;

  router.get("/written", [&refused](http::response_writer out) -> task<> {
    co_await out.write("before");

    try {
      http::event_writer const events{std::move(out)};
    } catch (std::logic_error const &) {
      ++refused;
    }
  });
  router.get("/moved", [&refused](http::response_writer out) -> task<> {
    auto kept = std::move(out);

    try {
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what is tested.
      http::event_writer const events{std::move(out)};
    } catch (std::logic_error const &) {
      ++refused;
    }

    co_await kept.write("kept");
  });

  REQUIRE(answer_to(http::server{router}, get("/written")).find("before") != std::string::npos);
  REQUIRE(answer_to(http::server{router}, get("/moved")).find("kept") != std::string::npos);
  REQUIRE(refused == 2);
}

TEST_CASE("event_writer: a Cache-Control of a middleware stays", "[server][sse]") {
  http::router router;

  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.fields.set("Cache-Control", "no-store");
    co_return answer;
  });
  router.get("/feed", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.send({.data = "x"});
  });

  auto const answer = answer_to(http::server{router}, get("/feed"));

  REQUIRE(answer.find("Cache-Control: no-store\r\n") != std::string::npos);
  REQUIRE(answer.find("Content-Type: text/event-stream\r\n") != std::string::npos);
}

TEST_CASE("event_writer: a Cache-Control and a status of the handler stay", "[server][sse]") {
  http::router router;

  router.get("/feed", [](http::response_writer out) -> task<> {
    out.status = http::status::accepted;
    out.fields.set("Cache-Control", "private, no-cache");

    http::event_writer events{std::move(out)};

    co_await events.send({.data = "x"});
  });

  auto const answer = answer_to(http::server{router}, get("/feed"));

  REQUIRE(answer.starts_with("HTTP/1.1 202"));
  REQUIRE(answer.find("Cache-Control: private, no-cache\r\n") != std::string::npos);
  REQUIRE(answer.find("no-store") == std::string::npos);
}

TEST_CASE("event_writer: an event format_event refuses throws before anything leaves", "[server][sse]") {
  http::router router;
  bool refused = false;

  router.get("/feed", [&refused](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    try {
      static_cast<void>(events.send({.type = "a\nb", .data = "x"}));
    } catch (std::invalid_argument const &) {
      refused = true;
    }

    co_await events.send({.data = "after"});
  });

  auto const answer = answer_to(http::server{router}, get("/feed"));

  REQUIRE(refused);
  REQUIRE(answer.find("\r\ndata: after\n\n\r\n") != std::string::npos);
  REQUIRE(answer.find("event:") == std::string::npos);
}
