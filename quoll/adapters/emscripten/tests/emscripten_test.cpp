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
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/emscripten.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/overloaded.hpp>

#include <catch2/catch_test_macros.hpp>

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

// tests/test_server.js
extern "C" {
void quoll_test_server_start();
int quoll_test_server_port();
void quoll_test_server_stop();
}

namespace {

namespace http = quoll::http;

using quoll::async::task;
using quoll::emscripten::fetch_transport;
using quoll::test::overloaded;

// Runs `work` on the event loop, suspending the test there (JSPI) until it completes, then returns its value or
// rethrows its exception. Past a time limit, requests stop on `source`, then fails the test; if even the stop request
// does not end the task, aborts: the task still refers to this frame.
template <class T>
T run(task<T> work, std::stop_source source = {}) {
  std::optional<T> result;
  std::exception_ptr failure;
  bool done = false;

  co_spawn(quoll::emscripten::executor{}, std::move(work),
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

// The Node HTTP server of tests/test_server.js, listening on a free port of the loopback interface while it lives.
class test_server {
public:
  test_server() {
    quoll_test_server_start();

    while (quoll_test_server_port() == 0) {
      emscripten_sleep(1);
    }

    port_ = quoll_test_server_port();
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

task<http::response> post(fetch_transport transport, std::string target, std::string body) {
  http::request request;

  request.method = http::verb::post;
  request.target = std::move(target);
  request.body = std::move(body);
  request.fields.add("Content-Length", std::to_string(request.body.size()));
  co_return co_await transport.send(std::move(request));
}

task<std::error_code> send_error(fetch_transport transport, std::string target) {
  http::request request;

  request.target = std::move(target);

  try {
    (void)co_await transport.send(std::move(request));
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

task<> request_stop(std::stop_source &source) {
  source.request_stop();
  co_return;
}

// Requests a target the server never answers, then requests stop: when_all starts the fetch first.
task<std::error_code> send_then_stop(fetch_transport transport, std::stop_source &source) {
  auto const [error, stopped] = co_await quoll::async::when_all(send_error(transport, "/hang"), request_stop(source));

  co_return error;
}

} // namespace

TEST_CASE("emscripten executor resumes coroutines on the event loop", "[emscripten]") {
  REQUIRE(run(count_yields(3)) == 3);
  REQUIRE(quoll::emscripten::executor{} == quoll::emscripten::executor{});
}

TEST_CASE("fetch_transport: a client GET, end to end", "[emscripten][http]") {
  test_server const server;
  auto const response = run(get(fetch_transport{server.url()}));

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.reason == "OK");
  REQUIRE(response.body == "hello over fetch");
  REQUIRE(response.fields.find("x-method") == "GET");
  REQUIRE(response.fields.find("x-target") == "/accounts/42");
  REQUIRE(response.fields.find("X-Test") == "from quoll");
}

TEST_CASE("fetch_transport: a body goes out and an error status comes back as a response", "[emscripten][http]") {
  test_server const server;
  auto const response = run(post(fetch_transport{server.url()}, "/status/404", "a body"));

  REQUIRE(response.status == http::status::not_found);
  REQUIRE(response.reason == "Not Found");
  REQUIRE(response.body == "a body");
  REQUIRE(response.fields.find("x-method") == "POST");
}

TEST_CASE("fetch_transport: a server nobody listens on fails", "[emscripten]") {
  std::string url;

  {
    test_server const server;

    url = server.url();
  }

  REQUIRE(run(send_error(fetch_transport{url}, "/")) == std::errc::io_error);
}

TEST_CASE("fetch_transport: a stop request aborts a pending fetch", "[emscripten]") {
  test_server const server;
  std::stop_source source;

  REQUIRE(run(send_then_stop(fetch_transport{server.url()}, source), source) == std::errc::operation_canceled);
}

TEST_CASE("fetch_transport: a stop request made before fetching", "[emscripten]") {
  test_server const server;
  std::stop_source source;

  source.request_stop();

  REQUIRE(run(send_error(fetch_transport{server.url()}, "/"), source) == std::errc::operation_canceled);
}
