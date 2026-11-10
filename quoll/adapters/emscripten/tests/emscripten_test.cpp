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
#include <quoll/async/current_executor.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/runtime_options.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/emscripten.hpp>
#include <quoll/emscripten/detail/js.hpp>
#include <quoll/emscripten/runtime.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/platform_ref.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/rest/api.hpp>
#include <quoll/rest/body.hpp>
#include <quoll/rest/client.hpp>
#include <quoll/rest/endpoint.hpp>
#include <quoll/rest/events.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/returns.hpp>
#include <quoll/rest/text_codec.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/reports.hpp>
#include <quoll/test/under_stop.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <emscripten.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// tests/test_server.js
extern "C" {
void quoll_test_server_start();
int quoll_test_server_port();
void quoll_test_server_stop();
void quoll_test_server_write(char const *text, std::size_t size);
void quoll_test_server_end();
void quoll_test_server_cut();
int quoll_test_server_closed();
int quoll_test_fetch_entries();
int quoll_test_fetch_held_bytes();
}

namespace {

namespace http = quoll::http;

using quoll::async::task;
using quoll::test::overloaded;

// Runs `work` on the event loop, suspending the test there (JSPI) until it completes, then returns its value or
// rethrows its exception. Past a time limit, requests stop on `source`, then fails the test; if even the stop request
// does not end the task, aborts: the task still refers to this frame.
template <class T>
T run_on(quoll::async::Executor auto executor, task<T> work, std::stop_source source = {}) {
  std::optional<T> result;
  std::exception_ptr failure;
  bool done = false;

  co_spawn(std::move(executor), std::move(work),
           overloaded{[&](T value) {
                        result = std::move(value);
                        done = true;
                      },
                      [&](std::exception_ptr error) {
                        failure = std::move(error);
                        done = true;
                      }},
           source.get_token());

  auto const start = emscripten_get_now();
  bool timed_out = false;

  while (!done) {
    auto const elapsed = emscripten_get_now() - start;

    if (!timed_out && elapsed > 10'000.0) {
      timed_out = true;
      source.request_stop();
    }

    if (elapsed > 20'000.0) {
      std::fputs("quoll test: a task ignored its stop request\n", stderr);
      std::abort();
    }

    emscripten_sleep(1);
  }

  REQUIRE_FALSE(timed_out);

  if (failure) {
    std::rethrow_exception(failure);
  }

  return std::move(*result);
}

template <class T>
T run(task<T> work, std::stop_source source = {}) {
  return run_on(quoll::emscripten::executor{}, std::move(work), std::move(source));
}

// The Node HTTP server of tests/test_server.js, listening on a free port of the loopback interface while it lives.
// Throws if it cannot listen within a time limit.
class test_server {
public:
  test_server() {
    quoll_test_server_start();

    auto const start = emscripten_get_now();

    while (quoll_test_server_port() == 0 && emscripten_get_now() - start < 10'000.0) {
      emscripten_sleep(1);
    }

    port_ = quoll_test_server_port();

    if (port_ <= 0) {
      quoll_test_server_stop();
      throw std::runtime_error{"quoll test server: cannot listen"};
    }
  }

  test_server(test_server const &) = delete;
  test_server(test_server &&) = delete;
  test_server &operator=(test_server const &) = delete;
  test_server &operator=(test_server &&) = delete;

  ~test_server() { quoll_test_server_stop(); }

  [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
  int port_ = 0;
};

task<int> count_yields(int yields) {
  for (int index = 0; index < yields; ++index) {
    co_await quoll::async::yield();
  }

  co_return yields;
}

// The client GET of the other transports, written against the concept.
template <http::HttpTransport Transport>
task<http::response> get(Transport const &transport) {
  http::request request;

  request.method = http::verb::get;
  request.target = "/accounts/42";
  request.fields.add("Host", "localhost");
  request.fields.add("X-Test", "from quoll");
  co_return co_await transport.send(std::move(request));
}

task<http::response> post(http::client transport, std::string target, std::string body) {
  http::request request;

  request.method = http::verb::post;
  request.target = std::move(target);
  request.body = std::move(body);
  request.fields.add("Content-Length", std::to_string(request.body.size()));
  co_return co_await transport.send(std::move(request));
}

task<http::response> get_target(http::client transport, std::string target, http::fields fields = {}) {
  http::request request;

  request.target = std::move(target);
  request.fields = std::move(fields);
  co_return co_await transport.send(std::move(request));
}

task<std::error_code> send_error(http::client transport, std::string target) {
  http::request request;

  request.target = std::move(target);

  co_return co_await quoll::test::code_thrown_by(transport.send(std::move(request)));
}

task<> request_stop(std::stop_source &source) {
  source.request_stop();
  co_return;
}

// Requests a target the server never answers, then requests stop: when_all starts the fetch first.
task<std::error_code> send_then_stop(http::client transport, std::stop_source &source) {
  auto const [error, stopped] = co_await quoll::async::when_all(send_error(transport, "/hang"), request_stop(source));

  co_return error;
}

// The milliseconds a sleep of `duration` takes.
task<double> measure_sleep(std::chrono::nanoseconds duration) {
  auto const start = emscripten_get_now();

  co_await quoll::async::sleep_for(duration);
  co_return emscripten_get_now() - start;
}

task<std::error_code> sleep_error(std::chrono::nanoseconds duration) {
  co_return co_await quoll::test::code_thrown_by(quoll::async::sleep_for(duration));
}

// Sleeps, then requests stop: when_all starts the sleep first.
task<std::error_code> sleep_then_stop(std::stop_source &source) {
  using namespace std::chrono_literals;

  auto const [error, stopped] = co_await quoll::async::when_all(sleep_error(1h), request_stop(source));

  co_return error;
}

// Requests a target the server never answers, up to the timeout.
task<std::error_code> fetch_with_timeout(http::client transport, std::chrono::nanoseconds timeout) {
  http::request request;

  request.target = "/hang";

  co_return co_await quoll::test::code_thrown_by(
      quoll::async::with_timeout(timeout, transport.send(std::move(request))));
}

} // namespace

TEST_CASE("emscripten timer: its clock goes by as it waits", "[emscripten][timer]") {
  auto const waiting = []() -> quoll::async::task<std::chrono::nanoseconds> {
    auto const executor = co_await quoll::async::current_executor();
    auto const timer = executor.query<quoll::async::timer_ref>();
    auto const started = timer->now();

    co_await quoll::async::sleep_for(std::chrono::milliseconds{20});
    co_return timer->now() - started;
  };

  // A wait lasts at least what it asks, give or take the rounding of what JavaScript reports.
  REQUIRE(run(waiting()) >= std::chrono::milliseconds{19});
}

TEST_CASE("emscripten executor offers a timer on the event loop", "[emscripten][timer]") {
  using namespace std::chrono_literals;

  // Node's timers run on a clock rounded to the millisecond: one may end just under its duration here.
  REQUIRE(run(measure_sleep(20ms)) >= 19.0);
}

TEST_CASE("emscripten timer: a stop request ends a sleep", "[emscripten][timer]") {
  std::stop_source source;

  REQUIRE(run(sleep_then_stop(source), source) == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: with_timeout aborts a pending fetch", "[emscripten][timer]") {
  using namespace std::chrono_literals;

  test_server const server;

  REQUIRE(run(fetch_with_timeout(http::client{server.url()}, 20ms)) == std::errc::timed_out);
}

TEST_CASE("emscripten executor resumes coroutines on the event loop", "[emscripten]") {
  REQUIRE(run(count_yields(3)) == 3);
  REQUIRE(quoll::emscripten::executor{} == quoll::emscripten::executor{});
}

TEST_CASE("http::client through fetch: a client GET, end to end", "[emscripten][http]") {
  test_server const server;
  auto const response = run(get(http::client{server.url()}));

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.reason == "OK");
  REQUIRE(response.body == "hello over fetch");
  REQUIRE(response.fields.find("x-method") == "GET");
  REQUIRE(response.fields.find("x-target") == "/accounts/42");
  REQUIRE(response.fields.find("X-Test") == "from quoll");
}

TEST_CASE("http::client through fetch: a body goes out and an error status comes back as a response",
          "[emscripten][http]") {
  test_server const server;
  auto const response = run(post(http::client{server.url()}, "/status/404", "a body"));

  REQUIRE(response.status == http::status::not_found);
  REQUIRE(response.reason == "Not Found");
  REQUIRE(response.body == "a body");
  REQUIRE(response.fields.find("x-method") == "POST");
}

TEST_CASE("http::client through fetch: a server nobody listens on fails", "[emscripten]") {
  std::string url;

  {
    test_server const server;

    url = server.url();
  }

  REQUIRE(run(send_error(http::client{url}, "/")) == std::errc::io_error);
}

TEST_CASE("http::client through fetch: a stop request aborts a pending fetch", "[emscripten]") {
  test_server const server;
  std::stop_source source;

  REQUIRE(run(send_then_stop(http::client{server.url()}, source), source) == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: a stop request made before fetching", "[emscripten]") {
  test_server const server;
  std::stop_source source;

  source.request_stop();

  REQUIRE(run(send_error(http::client{server.url()}, "/"), source) == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: a field that would split into others is rejected before fetching",
          "[emscripten]") {
  http::client const transport{"http://127.0.0.1"};
  http::request injected_value;
  http::request injected_name;

  injected_value.fields.add("X-Test", "a\nX-Injected:1");
  injected_name.fields.add("X-Test:a", "b");

  REQUIRE_THROWS_AS((void)transport.send(std::move(injected_value)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)transport.send(std::move(injected_name)), std::invalid_argument);
}

TEST_CASE("http::client through fetch: a target that could leave the origin is rejected before fetching",
          "[emscripten]") {
  http::client const transport{"http://127.0.0.1"};
  http::request other_host;
  http::request with_space;

  other_host.target = "@evil.example/";
  with_space.target = "/a b";

  REQUIRE_THROWS_AS((void)transport.send(std::move(other_host)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)transport.send(std::move(with_space)), std::invalid_argument);
}

TEST_CASE("http::client through fetch: a compressed body comes decoded, without the fields of the wire",
          "[emscripten][http]") {
  test_server const server;
  auto const response = run(get_target(http::client{server.url()}, "/gzip"));

  REQUIRE(response.body == "hello gzipped");
  REQUIRE_FALSE(response.fields.contains("Content-Encoding"));
  REQUIRE_FALSE(response.fields.contains("Content-Length"));
}

TEST_CASE("http::client through fetch: field values are bytes, both ways", "[emscripten][http]") {
  test_server const server;
  http::fields fields;

  // Latin-1 'é', not valid UTF-8 on its own.
  fields.add("X-Test", "caf\xE9");

  auto const response = run(get_target(http::client{server.url()}, "/", std::move(fields)));

  REQUIRE(response.fields.find("x-test") == "caf\xE9");
}

namespace {

// The Node test server as a third-party API, described by its contract only: no Quoll server behind it.
namespace node_server_api {
using namespace quoll::rest;

using echo = endpoint<"echo", http::verb::post, "/echo/{name}", header<"X-Test", std::string>,
                      body<std::string, text_codec>, returns<std::string, http::status::ok, text_codec>>;
using missing = endpoint<"missing", http::verb::get, "/status/404", returns<std::string, http::status::ok, text_codec>>;

// The events the test writes on the server's /events, as they come.
using events = endpoint<"events", http::verb::get, "/events", quoll::rest::events<std::string, text_codec>>;

using api = quoll::rest::api<"", echo, missing, events>;
} // namespace node_server_api

} // namespace

TEST_CASE("http::client through fetch: a body_writer goes out whole", "[emscripten][http]") {
  test_server const server;
  http::client const transport{server.url()};
  http::request request;

  request.method = http::verb::post;
  request.target = "/status/200";
  request.body_writer = [](http::body_stream out) -> quoll::async::task<> {
    co_await out.write("one ");
    co_await out.write("two");
  };

  REQUIRE(run(transport.send(std::move(request))).body == "one two");

  http::request both;

  both.method = http::verb::post;
  both.target = "/status/200";
  both.body = "whole";
  both.body_writer = [](http::body_stream) -> quoll::async::task<> { co_return; };
  REQUIRE_THROWS_AS(transport.send(std::move(both)), std::invalid_argument);

  http::request short_of_it;

  short_of_it.method = http::verb::post;
  short_of_it.target = "/status/200";
  short_of_it.fields.set("Content-Length", "1000");
  short_of_it.body_writer = [](http::body_stream out) -> quoll::async::task<> { co_await out.write("four"); };
  REQUIRE_THROWS_AS(run(transport.send(std::move(short_of_it))), std::length_error);
}

namespace {

// The event loop's executor, that asks for a stop first once armed: what comes then is what a fetch that has settled
// posts, just before what waits for it resumes.
class stopping_executor {
public:
  // The event loop's fetch, and no timer: nothing else posts between a response and what it wakes.
  using services = quoll::async::service_list<http::platform_ref>;

  stopping_executor(std::stop_source &source, bool &armed) noexcept : source_(&source), armed_(&armed) {}

  [[nodiscard]] http::platform_ref query(quoll::async::service_tag<http::platform_ref> tag) const {
    return quoll::emscripten::executor{}.query(tag);
  }

  void post(quoll::async::work_item &item) const noexcept {
    if (*armed_) {
      *armed_ = false;
      source_->request_stop();
    }

    quoll::emscripten::executor{}.post(item);
  }

  [[nodiscard]] bool operator==(stopping_executor const &) const noexcept = default;

private:
  std::stop_source *source_;
  bool *armed_;
};

static_assert(quoll::async::Executor<stopping_executor>);

// All of `body`, under no stop, whatever the task reading it was told.
task<std::string> read_unstopped(http::response_body const &body QUOLL_LIFETIMEBOUND) {
  std::string all;

  for (;;) {
    auto const piece = co_await quoll::test::under_stop(body.read(), std::stop_token{});

    if (!piece) {
      co_return all;
    }

    all += *piece;
  }
}

} // namespace

TEST_CASE("http::client through fetch: a stop just after a response has come leaves it whole",
          "[emscripten][http][stream]") {
  test_server const server;
  http::client const transport{server.url()};
  std::stop_source source;
  auto armed = false;

  auto const reading = [&]() -> task<std::string> {
    http::request request;

    request.method = http::verb::post;
    request.target = "/status/200";
    request.body = "whole";

    armed = true;

    auto response = co_await transport.stream(std::move(request));

    co_return co_await read_unstopped(response.body);
  };

  REQUIRE(run_on(stopping_executor{source, armed}, reading(), source) == "whole");
  REQUIRE(source.stop_requested());
}

TEST_CASE("http::client through fetch: a stop just after a piece has come leaves the body whole",
          "[emscripten][http][stream]") {
  test_server const server;
  http::client const transport{server.url()};
  std::stop_source source;
  auto armed = false;

  auto const reading = [&]() -> task<std::string> {
    http::request request;

    request.method = http::verb::post;
    request.target = "/status/200";
    request.body = "whole";

    auto response = co_await transport.stream(std::move(request));

    // The read under the stop that comes, the rest under none.
    armed = true;

    auto const first = co_await response.body.read();

    if (!first) {
      co_return std::string{"no piece"};
    }

    auto all = std::string{*first};

    co_return all + co_await read_unstopped(response.body);
  };

  REQUIRE(run_on(stopping_executor{source, armed}, reading(), source) == "whole");
  REQUIRE(source.stop_requested());
}

TEST_CASE("http::client through fetch: stream() takes a body_writer too, gathered first",
          "[emscripten][http][stream]") {
  test_server const server;
  http::client const transport{server.url()};
  http::request request;

  request.method = http::verb::post;
  request.target = "/status/200";
  request.body_writer = [](http::body_stream out) -> quoll::async::task<> {
    co_await out.write("one ");
    co_await out.write("two");
  };

  auto const reading = [&]() -> quoll::async::task<std::string> {
    auto response = co_await transport.stream(std::move(request));
    std::string body;

    while (auto const piece = co_await response.body.read()) {
      body += *piece;
    }

    co_return body;
  };

  REQUIRE(run(reading()) == "one two");

  http::request both;

  both.method = http::verb::post;
  both.target = "/status/200";
  both.body = "whole";
  both.body_writer = [](http::body_stream) -> quoll::async::task<> { co_return; };
  REQUIRE_THROWS_AS(transport.stream(std::move(both)), std::invalid_argument);

  http::request short_of_it;

  short_of_it.method = http::verb::post;
  short_of_it.target = "/status/200";
  short_of_it.fields.set("Content-Length", "1000");
  short_of_it.body_writer = [](http::body_stream out) -> quoll::async::task<> { co_await out.write("four"); };
  REQUIRE_THROWS_AS(run(transport.stream(std::move(short_of_it))), std::length_error);

  http::request endless;

  endless.method = http::verb::post;
  endless.target = "/status/200";
  endless.body_writer = [](http::body_stream out) -> quoll::async::task<> {
    for (;;) {
      try {
        co_await quoll::async::sleep_for(std::chrono::milliseconds{1});
      } catch (std::system_error const &) {
      }

      co_await out.write("x");
    }
  };

  auto const timing_out = [&]() -> quoll::async::task<std::error_code> {
    try {
      static_cast<void>(
          co_await quoll::async::with_timeout(std::chrono::milliseconds{20}, transport.stream(std::move(endless))));
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(run(timing_out()) == std::make_error_code(std::errc::timed_out));
}

TEST_CASE("http::client through fetch: a timeout ends a body_writer at its next write", "[emscripten][http][timer]") {
  test_server const server;
  http::client const transport{server.url()};
  http::request request;

  request.method = http::verb::post;
  request.target = "/status/200";
  request.body_writer = [](http::body_stream out) -> quoll::async::task<> {
    // Its own wait lets the event loop run the timeout, and what it then writes is refused, whatever it makes of the
    // wait.
    for (;;) {
      try {
        co_await quoll::async::sleep_for(std::chrono::milliseconds{1});
      } catch (std::system_error const &) {
      }

      co_await out.write("x");
    }
  };

  auto const timing_out = [&]() -> quoll::async::task<std::error_code> {
    try {
      static_cast<void>(
          co_await quoll::async::with_timeout(std::chrono::milliseconds{20}, transport.send(std::move(request))));
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(run(timing_out()) == std::make_error_code(std::errc::timed_out));
}

TEST_CASE("rest::client over http::client calls an API that only has a contract", "[emscripten][rest]") {
  test_server const server;
  quoll::rest::client<node_server_api::api> const client{server.url()};

  REQUIRE(run(client.call<node_server_api::echo>("a b", "from the contract", "the body")) == "the body");

  auto const missing = run(client.try_call<node_server_api::missing>());

  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().response.status == http::status::not_found);
  REQUIRE(missing.error().reason == "undeclared status 404");
}

namespace {

task<http::streamed_response> stream_target(http::client transport, std::string target,
                                            http::verb method = http::verb::get, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  request.body = std::move(body);
  co_return co_await transport.stream(std::move(request));
}

task<std::error_code> stream_error(http::client transport, std::string target) {
  co_return co_await quoll::test::code_thrown_by(stream_target(std::move(transport), std::move(target)));
}

// Requests a target the server never answers, then requests stop: when_all starts the fetch first.
task<std::error_code> stream_then_stop(http::client transport, std::stop_source &source) {
  auto const [error, stopped] =
      co_await quoll::async::when_all(stream_error(std::move(transport), "/hang"), request_stop(source));

  co_return error;
}

// The next piece, copied as what the view shows does not outlive the task's result.
task<std::optional<std::string>> next_piece(http::response_body const &body) {
  auto const piece = co_await body.read();

  co_return piece ? std::optional<std::string>{*piece} : std::nullopt;
}

task<std::string> read_rest(http::response_body const &body) {
  std::string rest;

  while (auto const piece = co_await body.read()) {
    rest += *piece;
  }

  co_return rest;
}

// What read() throws, or a default error if it does not.
task<std::system_error> read_failure(http::response_body const &body) {
  try {
    (void)co_await body.read();
  } catch (std::system_error const &error) {
    co_return error;
  }

  co_return std::system_error{std::error_code{}};
}

// Reads what has not been written yet, then requests stop: when_all starts the read first.
task<std::system_error> read_then_stop(http::response_body const &body, std::stop_source &source) {
  auto const [failure, stopped] = co_await quoll::async::when_all(read_failure(body), request_stop(source));

  co_return failure;
}

task<std::error_code> read_with_timeout(http::response_body const &body, std::chrono::nanoseconds timeout) {
  co_return co_await quoll::test::code_thrown_by(quoll::async::with_timeout(timeout, body.read()));
}

void server_writes(std::string_view text) { quoll_test_server_write(text.data(), text.size()); }

// Whether the connection of /open closes on the server's side within a time limit, on the event loop.
bool server_sees_close() {
  auto const start = emscripten_get_now();

  while (quoll_test_server_closed() == 0 && emscripten_get_now() - start < 5'000.0) {
    emscripten_sleep(1);
  }

  return quoll_test_server_closed() != 0;
}

// Long enough for a close to reach the server, were one on its way.
void let_the_event_loop_run() { emscripten_sleep(50); }

} // namespace

TEST_CASE("http::client through fetch: stream completes before the body, and hands it over as it comes",
          "[emscripten][stream]") {
  test_server const server;
  // The server has written nothing of the body: waiting for it would never end, and fail by the time limit of run().
  auto const response = run(stream_target(http::client{server.url()}, "/open"));

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.fields.find("x-open") == "yes");

  server_writes("first");
  REQUIRE(run(next_piece(response.body)) == "first");
  // Copied, and no longer held in JavaScript as well.
  REQUIRE(quoll_test_fetch_held_bytes() == 0);

  server_writes("second");
  REQUIRE(run(next_piece(response.body)) == "second");

  quoll_test_server_end();
  REQUIRE(run(next_piece(response.body)) == std::nullopt);
}

TEST_CASE("event_reader through fetch: events as they come, written in the middle of a field and of a CRLF",
          "[emscripten][stream][sse]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));
  http::event_reader events{response.body};

  server_writes("id: 1\ndata: first\n\n");
  REQUIRE(run(events.next()) == http::server_sent_event{.data = "first", .id = "1"});

  // Written in the middle of a field, and of a CRLF: how fetch hands them over is its own, the parser's tests cut
  // every input a byte at a time.
  server_writes("event: add\r\ndata: sec");
  emscripten_sleep(20);
  server_writes("ond\r");
  emscripten_sleep(20);
  server_writes("\n\r\n");
  REQUIRE(run(events.next()) == http::server_sent_event{.type = "add", .data = "second", .id = "1"});

  server_writes("data: lost");
  quoll_test_server_end();
  REQUIRE(run(events.next()) == std::nullopt);
  REQUIRE(events.last_event_id() == "1");
}

TEST_CASE("rest::client through fetch: subscribe reads the events as the server writes them",
          "[emscripten][rest][sse]") {
  test_server const server;
  quoll::rest::client<node_server_api::api> const client{server.url()};
  auto stream = run(client.subscribe<node_server_api::events>());

  server_writes("id: 1\ndata: one\n\n");
  REQUIRE(run(stream.next()) == "one");

  server_writes("data: tw");
  emscripten_sleep(20);
  server_writes("o\n\n");
  REQUIRE(run(stream.next()) == "two");

  quoll_test_server_end();
  REQUIRE(run(stream.next()) == std::nullopt);
  REQUIRE(stream.last_event_id() == "1");
}

TEST_CASE("http::client through fetch: stream gives several pieces, none larger than what was written at once",
          "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));
  std::vector<std::string> const writes{"one", "two", "three", std::string(1 << 20, 'x')};
  std::vector<std::size_t> pieces;

  for (auto const &written : writes) {
    std::string received;

    server_writes(written);
    pieces.push_back(0);

    while (received.size() < written.size()) {
      auto const piece = run(next_piece(response.body));

      REQUIRE(piece.has_value());
      REQUIRE(piece->size() <= written.size());
      received += *piece;
      ++pieces.back();
    }

    REQUIRE(received == written);
  }

  // Nothing holds the large one whole.
  REQUIRE(pieces.back() > 1);

  quoll_test_server_end();
  REQUIRE(run(next_piece(response.body)) == std::nullopt);
}

TEST_CASE("http::client through fetch: stream gives the status, status text and fields of send",
          "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/status/404", http::verb::post, "a body"));

  REQUIRE(response.status == http::status::not_found);
  REQUIRE(response.reason == "Not Found");
  REQUIRE(response.version == http::version::http_1_1);
  REQUIRE(response.fields.find("x-method") == "POST");
  REQUIRE(response.fields.find("x-target") == "/status/404");
  REQUIRE(run(read_rest(response.body)) == "a body");
  REQUIRE(response.trailers != nullptr);
  REQUIRE(response.trailers->empty());
}

TEST_CASE("http::client through fetch: stream decodes a body, without the fields of the wire", "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/gzip"));

  REQUIRE_FALSE(response.fields.contains("Content-Encoding"));
  REQUIRE_FALSE(response.fields.contains("Content-Length"));
  REQUIRE(run(read_rest(response.body)) == "hello gzipped");
}

TEST_CASE("http::client through fetch: stream of an empty body frees its fetch at the end", "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));

  REQUIRE(quoll_test_fetch_entries() == 1);

  quoll_test_server_end();
  REQUIRE(run(next_piece(response.body)) == std::nullopt);
  // With the response still here, and however often it is asked.
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(run(next_piece(response.body)) == std::nullopt);
}

TEST_CASE("http::client through fetch: stream with no body at all ends at once", "[emscripten][stream]") {
  test_server const server;
  auto const [method, target] = GENERATE(table<http::verb, std::string>({
      {http::verb::head, "/"},
      {http::verb::get, "/status/204"},
  }));
  auto const response = run(stream_target(http::client{server.url()}, target, method));

  REQUIRE(response.fields.find("x-target") == target);
  // Before any read: nothing will come.
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(run(next_piece(response.body)) == std::nullopt);
}

TEST_CASE("http::client through fetch: a body cut by the server fails with io_error, after what came",
          "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));

  server_writes("before");
  REQUIRE(run(next_piece(response.body)) == "before");

  quoll_test_server_cut();

  auto const failure = run(read_failure(response.body));

  REQUIRE(failure.code() == std::errc::io_error);
  REQUIRE_THAT(failure.what(), Catch::Matchers::ContainsSubstring("terminated"));
  // Reading ended there: the same again, with the entry gone though the response is here.
  REQUIRE(quoll_test_fetch_entries() == 0);

  auto const again = run(read_failure(response.body));

  REQUIRE(again.code() == std::errc::io_error);
  REQUIRE(std::string_view{again.what()} == failure.what());
}

TEST_CASE("http::client through fetch: a stop request during a read ends the reading", "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));
  std::stop_source source;

  REQUIRE(run(read_then_stop(response.body, source), source).code() == std::errc::operation_canceled);
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(server_sees_close());

  // Without a stop request now: the reading is over, not this read alone.
  REQUIRE(run(read_failure(response.body)).code() == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: a stop request made before a read ends the reading", "[emscripten][stream]") {
  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));
  std::stop_source source;

  source.request_stop();

  REQUIRE(run(read_failure(response.body), source).code() == std::errc::operation_canceled);
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(server_sees_close());
  REQUIRE(run(read_failure(response.body)).code() == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: with_timeout on a read ends the reading", "[emscripten][stream][timer]") {
  using namespace std::chrono_literals;

  test_server const server;
  auto const response = run(stream_target(http::client{server.url()}, "/open"));

  REQUIRE(run(read_with_timeout(response.body, 20ms)) == std::errc::timed_out);
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(server_sees_close());
  REQUIRE(run(read_failure(response.body)).code() == std::errc::operation_canceled);
}

TEST_CASE("http::client through fetch: dropping a response before the end aborts its fetch", "[emscripten][stream]") {
  test_server const server;
  auto const dropped_at = GENERATE(0, 1);

  {
    auto const response = run(stream_target(http::client{server.url()}, "/open"));

    if (dropped_at == 1) {
      server_writes("first");
      REQUIRE(run(next_piece(response.body)) == "first");
    }

    let_the_event_loop_run();
    REQUIRE(quoll_test_server_closed() == 0);
    REQUIRE(quoll_test_fetch_entries() == 1);
  }

  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(server_sees_close());
}

TEST_CASE("http::client through fetch: copies of a body share the reading, and the last to go aborts",
          "[emscripten][stream]") {
  test_server const server;
  std::optional<http::streamed_response> response{run(stream_target(http::client{server.url()}, "/open"))};
  std::optional<http::response_body> copy{response->body};

  server_writes("one");
  REQUIRE(run(next_piece(response->body)) == "one");

  server_writes("two");
  REQUIRE(run(next_piece(*copy)) == "two");

  // The response goes, and the copy holds the fetch.
  response.reset();
  let_the_event_loop_run();
  REQUIRE(quoll_test_server_closed() == 0);
  REQUIRE(quoll_test_fetch_entries() == 1);

  server_writes("three");
  REQUIRE(run(next_piece(*copy)) == "three");

  copy.reset();
  REQUIRE(quoll_test_fetch_entries() == 0);
  REQUIRE(server_sees_close());
}

TEST_CASE("http::client through fetch: a body moved from has nothing to read", "[emscripten][stream]") {
  test_server const server;
  auto response = run(stream_target(http::client{server.url()}, "/open"));
  auto const moved = std::move(response.body);

  REQUIRE(run(next_piece(response.body)) == std::nullopt);
  REQUIRE(quoll_test_fetch_entries() == 1);

  server_writes("still here");
  REQUIRE(run(next_piece(moved)) == "still here");
}

TEST_CASE("http::client through fetch: stream fails as send does", "[emscripten][stream]") {
  SECTION("a server nobody listens on") {
    std::string url;

    {
      test_server const server;

      url = server.url();
    }

    REQUIRE(run(stream_error(http::client{url}, "/")) == std::errc::io_error);
  }

  SECTION("a stop request during the fetch") {
    test_server const server;
    std::stop_source source;

    REQUIRE(run(stream_then_stop(http::client{server.url()}, source), source) == std::errc::operation_canceled);
  }

  SECTION("a stop request before the fetch") {
    test_server const server;
    std::stop_source source;

    source.request_stop();

    REQUIRE(run(stream_error(http::client{server.url()}, "/"), source) == std::errc::operation_canceled);
  }

  REQUIRE(quoll_test_fetch_entries() == 0);
}

TEST_CASE("http::client through fetch: stream rejects before fetching what send rejects", "[emscripten][stream]") {
  http::client const transport{"http://127.0.0.1"};
  http::request injected_value;
  http::request other_host;
  http::request with_space;

  injected_value.fields.add("X-Test", "a\nX-Injected:1");
  other_host.target = "@evil.example/";
  with_space.target = "/a b";

  REQUIRE_THROWS_AS((void)transport.stream(std::move(injected_value)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)transport.stream(std::move(other_host)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)transport.stream(std::move(with_space)), std::invalid_argument);
}

TEST_CASE("http::client through fetch: the shortcuts", "[emscripten][http]") {
  test_server const server;
  http::client const client{server.url()};

  auto const got = run(client.get("/accounts/42"));

  REQUIRE(got.status == http::status::ok);
  REQUIRE(got.fields.find("x-method") == "GET");
  REQUIRE(got.fields.find("x-target") == "/accounts/42");

  auto const posted = run(client.post("/status/404", "a body", "text/plain"));

  REQUIRE(posted.status == http::status::not_found);
  REQUIRE(posted.body == "a body");
  REQUIRE(posted.fields.find("x-method") == "POST");
}

TEST_CASE("http::client through fetch: a path in its URL goes before each target", "[emscripten][http]") {
  test_server const server;
  http::client const client{server.url() + "/v1"};
  auto const got = run(client.get("/accounts/42"));

  REQUIRE(got.fields.find("x-target") == "/v1/accounts/42");
}

TEST_CASE("http::client through fetch: its options still count where they can", "[emscripten][http]") {
  using namespace std::chrono_literals;

  test_server const server;

  auto const error_of = [](http::client client, std::string target) -> task<std::error_code> {
    try {
      (void)co_await client.get(std::move(target));
    } catch (std::system_error const &failure) {
      co_return failure.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(run(error_of(http::client{server.url(), {.limits = {.max_body_size = 2}}}, "/accounts/42")) ==
          http::error::body_too_large);
  REQUIRE(run(error_of(http::client{server.url(), {.response_timeout = 20ms}}, "/hang")) == std::errc::timed_out);
  // What the browser decides has no effect, rather than refusing to run.
  REQUIRE_FALSE(run(error_of(http::client{server.url(), {.max_connections = 1, .max_redirects = 3}}, "/accounts/42")));
}

TEST_CASE("http::client through fetch: a streamed body keeps to its limit", "[emscripten][http]") {
  test_server const server;

  auto const read_all = [](http::client client) -> task<std::error_code> {
    http::request request;

    request.target = "/accounts/42";

    auto response = co_await client.stream(std::move(request));

    try {
      while (co_await response.body.read()) {
      }
    } catch (std::system_error const &failure) {
      co_return failure.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(run(read_all(http::client{server.url(), {.limits = {.max_streamed_body_size = 2}}})) ==
          http::error::body_too_large);
}

TEST_CASE("http::client through fetch: a URL relative to the page is taken", "[emscripten][http]") {
  REQUIRE_NOTHROW(http::client{""});
  REQUIRE_NOTHROW(http::client{"/api/v1"});
  REQUIRE_THROWS_AS(http::client{"/api/../v1"}, std::invalid_argument);
  REQUIRE_THROWS_AS(http::client{"ftp://example.com"}, std::invalid_argument);
}

namespace {

// Says where a request would have gone.
struct recording_platform {
  std::string *url;

  [[nodiscard]] task<http::streamed_response> stream(std::string where, http::request) const {
    *url = std::move(where);
    co_return http::streamed_response{};
  }
};

class recording_executor {
public:
  using services = quoll::async::service_list<http::platform_ref>;

  explicit recording_executor(recording_platform const &platform) noexcept : platform_(&platform) {}

  void post(quoll::async::work_item &item) const noexcept { quoll::emscripten::executor{}.post(item); }

  [[nodiscard]] http::platform_ref query(quoll::async::service_tag<http::platform_ref>) const {
    return http::platform_ref{*platform_};
  }

  [[nodiscard]] bool operator==(recording_executor const &) const noexcept = default;

private:
  recording_platform const *platform_;
};

} // namespace

TEST_CASE("http::client through fetch: where its URL sends a request", "[emscripten][http]") {
  std::string url;
  recording_platform const platform{&url};

  auto const where = [&](std::string const &base) {
    auto const send = [](http::client client) -> task<int> {
      (void)co_await client.get("/x?y=1");
      co_return 1;
    };

    url.clear();
    (void)run_on(recording_executor{platform}, send(http::client{base}));
    return url;
  };

  REQUIRE(where("") == "/x?y=1");
  REQUIRE(where("/api/v1") == "/api/v1/x?y=1");
  REQUIRE(where("/api/v1/") == "/api/v1/x?y=1");
  REQUIRE(where("http://example.com") == "http://example.com/x?y=1");
  REQUIRE(where("https://example.com:8443/v1") == "https://example.com:8443/v1/x?y=1");
}

TEST_CASE("http::client through fetch: a limit passed is passed for good", "[emscripten][http][stream]") {
  test_server const server;

  auto const read_twice = [](http::client client) -> task<std::pair<std::error_code, std::error_code>> {
    auto response = co_await client.stream([] {
      http::request request;

      request.target = "/accounts/42";
      return request;
    }());
    std::pair<std::error_code, std::error_code> errors;

    try {
      while (co_await response.body.read()) {
      }
    } catch (std::system_error const &failure) {
      errors.first = failure.code();
    }

    try {
      (void)co_await response.body.read();
    } catch (std::system_error const &failure) {
      errors.second = failure.code();
    }

    co_return errors;
  };

  auto const [first, second] = run(read_twice(http::client{server.url(), {.limits = {.max_streamed_body_size = 2}}}));

  REQUIRE(first == http::error::body_too_large);
  REQUIRE(second == http::error::body_too_large);
}

TEST_CASE("http::client through fetch: a body that stops coming times out, for good", "[emscripten][http][stream]") {
  using namespace std::chrono_literals;

  test_server const server;

  auto const read_twice = [](http::client client) -> task<std::pair<std::error_code, std::error_code>> {
    auto response = co_await client.stream([] {
      http::request request;

      request.target = "/open";
      return request;
    }());
    std::pair<std::error_code, std::error_code> errors;

    try {
      (void)co_await response.body.read();
    } catch (std::system_error const &failure) {
      errors.first = failure.code();
    }

    try {
      (void)co_await response.body.read();
    } catch (std::system_error const &failure) {
      errors.second = failure.code();
    }

    co_return errors;
  };

  auto const [first, second] = run(read_twice(http::client{server.url(), {.response_timeout = 200ms}}));

  REQUIRE(first == std::errc::timed_out);
  REQUIRE(second == std::errc::timed_out);
}

TEST_CASE("http::client through fetch: the time a writer takes is its own", "[emscripten][http]") {
  using namespace std::chrono_literals;

  test_server const server;
  http::client const client{server.url(), {.response_timeout = 100ms}};
  http::request request;

  request.method = http::verb::post;
  request.target = "/status/200";
  request.body_writer = [](http::body_stream out) -> task<> {
    co_await quoll::async::sleep_for(60ms);
    co_await out.write("slow ");
    co_await quoll::async::sleep_for(60ms);
    co_await out.write("writer");
  };

  REQUIRE(run(client.send(std::move(request))).body == "slow writer");
}

TEST_CASE("http::client through fetch: the head has its limits too", "[emscripten][http]") {
  test_server const server;

  auto const error_of = [](http::client client) -> task<std::error_code> {
    try {
      (void)co_await client.get("/accounts/42");
    } catch (std::system_error const &failure) {
      co_return failure.code();
    }

    co_return std::error_code{};
  };

  REQUIRE(run(error_of(http::client{server.url(), {.limits = {.max_field_count = 1}}})) ==
          http::error::header_too_large);
  REQUIRE(run(error_of(http::client{server.url(), {.limits = {.max_header_size = 4}}})) ==
          http::error::header_too_large);
}

namespace {

// Offers nothing, as an executor of an application may.
class bare_executor {
public:
  void post(quoll::async::work_item &item) const noexcept { quoll::emscripten::executor{}.post(item); }

  [[nodiscard]] bool operator==(bare_executor const &) const noexcept = default;
};

} // namespace

TEST_CASE("http::client through fetch: a relative URL needs an executor offering it", "[emscripten]") {
  auto const send = [](http::client client) -> task<int> {
    (void)co_await client.get("/x");
    co_return 1;
  };

  REQUIRE_THROWS_AS(run_on(bare_executor{}, send(http::client{"/api"})), std::logic_error);
}

TEST_CASE("http::client through fetch: the status text counts toward the head's limit", "[emscripten][http]") {
  test_server const server;

  auto const head_size = [](http::client client) -> task<std::size_t> {
    auto const response = co_await client.get("/accounts/42");
    std::size_t size = 0;

    for (auto const &entry : response.fields) {
      size += entry.name.size() + entry.value.size();
    }

    co_return size;
  };

  auto const error_of = [](http::client client) -> task<std::error_code> {
    try {
      (void)co_await client.get("/accounts/42");
    } catch (std::system_error const &failure) {
      co_return failure.code();
    }

    co_return std::error_code{};
  };

  // Room for the fields, and for one byte of "OK".
  auto const fields = run(head_size(http::client{server.url()}));

  REQUIRE_FALSE(run(error_of(http::client{server.url(), {.limits = {.max_header_size = fields + 2}}})));
  REQUIRE(run(error_of(http::client{server.url(), {.limits = {.max_header_size = fields + 1}}})) ==
          http::error::header_too_large);
}

TEST_CASE("emscripten::runtime: run(task) reports what the task let escape, and fails a Node process", "[emscripten]") {
  quoll::emscripten::runtime runtime;
  auto const failing = []() -> task<> {
    co_await quoll::async::yield();
    throw std::runtime_error{"no server"};
  };

  REQUIRE(quoll::emscripten::detail::quoll_emscripten_exit_code() == 0);
  REQUIRE(runtime.run(failing()) == EXIT_SUCCESS);

  auto const start = emscripten_get_now();

  while (quoll::test::reports().empty() && emscripten_get_now() - start < 10'000.0) {
    emscripten_sleep(1);
  }

  auto const failed = quoll::emscripten::detail::quoll_emscripten_exit_code();

  // Given back, or this test binary would end failing.
  quoll::emscripten::detail::quoll_emscripten_set_exit_code(0);
  REQUIRE(quoll::test::reports() ==
          std::vector<std::string>{"quoll: the program's main task ended on an exception: no server\n"});
  REQUIRE(failed == EXIT_FAILURE);
}

TEST_CASE("emscripten::runtime takes the options of every runtime, and runs its tasks", "[emscripten]") {
  // Four asked for: the browser runs one, and the build has no threads to tell apart.
  quoll::emscripten::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  auto const answer = []() -> task<int> {
    co_await quoll::async::yield();
    co_return 42;
  };

  std::optional<int> result;

  co_spawn(runtime.get_executor(), answer(),
           overloaded{[&result](int value) { result = value; }, [](std::exception_ptr) {}});
  runtime.run();

  // run() returns at once, and the browser's loop runs the task from then on.
  REQUIRE_FALSE(result);

  auto const start = emscripten_get_now();

  while (!result && emscripten_get_now() - start < 10'000.0) {
    emscripten_sleep(1);
  }

  REQUIRE(result == 42);
}
