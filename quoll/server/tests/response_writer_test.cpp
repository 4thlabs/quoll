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
#include <quoll/http/local_transport.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
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
using quoll::test::virtual_time;
using namespace std::chrono_literals;

task<std::string> read_until_closed(io::memory_stream &stream) {
  std::string received;
  std::array<char, 256> chunk;

  for (;;) {
    auto const result = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (result.error || result.transferred == 0) {
      co_return received;
    }

    received.append(chunk.data(), result.transferred);
  }
}

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

TEST_CASE("response_writer: to HEAD, what asks for a body gets no 100 Continue after the head", "[server]") {
  http::router router;

  router.get("/rows", [](http::body_reader in, http::response_writer out) -> task<> {
    try {
      co_await out.write("never sent");
    } catch (std::system_error const &) {
      // Reads on, once its body was refused.
    }

    (void)co_await in.read();
  });

  auto const answer = answer_to(
      http::server{router},
      "HEAD /rows HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n");

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("100 Continue") == std::string::npos);
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

    answer.body_writer = [&written, inner = std::move(answer.body_writer)](http::body_stream out) -> task<> {
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

TEST_CASE("response_writer: an answer replaced leaves what is running alone", "[server]") {
  http::router router;
  std::string label;

  router.use([&label](http::request request, http::next const &next) -> task<http::response> {
    auto answer = co_await next(std::move(request));

    answer.body_writer = [&label, inner = std::move(answer.body_writer),
                          kept = std::string(100, 'x')](http::body_stream out) -> task<> {
      try {
        co_await inner(out);
      } catch (std::exception const &) {
        // Reads what it captured, once the response has been replaced.
        label = kept;
        throw;
      }
    };
    co_return answer;
  });
  router.get("/early", [](http::response_writer out) -> task<> {
    out.status = http::status::continue_;
    co_await out.write("never sent");
  });

  REQUIRE(answer_to(http::server{router}, get("/early")).starts_with("HTTP/1.1 500"));
  REQUIRE(label == std::string(100, 'x'));
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
      // NOLINTNEXTLINE(bugprone-use-after-move)
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
  auto const writer = response.value().body_writer;

  (void)quoll::test::run(loop, [&]() -> task<int> {
    co_await writer(http::body_stream{sink});
    co_return 1;
  }());

  REQUIRE(read_back == std::string(100, 'y'));
}
