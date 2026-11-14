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

#include <quoll/async/task.hpp>
#include <quoll/http/ensure_ok.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/problem.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace http = quoll::http;

using quoll::async::task;
using quoll::http::status;
using quoll::http::verb;

namespace {

http::response dispatch(http::router const &router, verb method, std::string target) {
  http::request message;

  message.method = method;
  message.target = std::move(target);
  return quoll::test::run(router.dispatch(std::move(message))).value();
}

http::response answered_with(status code, std::string body) {
  http::response answer;

  answer.status = code;
  answer.body = std::move(body);
  return answer;
}

} // namespace

TEST_CASE("http_error: a handler raising one answers its status and a problem", "[http][http_error]") {
  http::router router;

  router.get("/notes/{id}", [](std::int64_t id) -> http::response {
    throw http::http_error{status::not_found, "no note " + std::to_string(id)};
  });

  auto const answer = dispatch(router, verb::get, "/notes/3");

  CHECK(answer.status == status::not_found);
  CHECK(answer.fields.find("Content-Type") == http::problem_content_type);
  CHECK(answer.body == R"({"title":"Not Found","status":404,"detail":"no note 3"})");
}

TEST_CASE("http_error: one received from another call, let through, answers 502", "[http][http_error]") {
  http::router router;

  router.get("/copy", []() -> http::response {
    throw http::http_error::received_from(answered_with(status::not_found, "theirs"), "their detail");
  });

  auto const answer = dispatch(router, verb::get, "/copy");

  CHECK(answer.status == status::bad_gateway);
  CHECK(answer.body.find("their") == std::string::npos);
  CHECK(answer.body.find("404") == std::string::npos);
}

TEST_CASE("http_error: a status that is no error goes on, as any exception", "[http][http_error]") {
  http::router router;

  router.get("/fine", []() -> http::response { throw http::http_error{status::ok, "not an error"}; });

  CHECK_THROWS_AS(dispatch(router, verb::get, "/fine"), http::http_error);
}

TEST_CASE("http_error: a middleware sees the problem as a response", "[http][http_error]") {
  http::router router;
  status seen = status::ok;

  router.get("/taken", []() -> http::response { throw http::http_error{status::conflict, "taken"}; });
  router.use([&seen](http::request message, http::next next) -> task<http::response> {
    auto answer = co_await next(std::move(message));

    seen = answer.status;
    co_return answer;
  });

  CHECK(dispatch(router, verb::get, "/taken").status == status::conflict);
  CHECK(seen == status::conflict);
}

TEST_CASE("problem_response: its detail escaped as JSON, what is not UTF-8 shown as '?'", "[http][http_error]") {
  CHECK(
      http::problem_response(status::bad_request, std::string{"a \"quote\", a \\ and\na\x01"}).body ==
      R"({"title":"Bad Request","status":400,"detail":"a \"quote\", a \\ and\na\u0001"})"
  );
  CHECK(
      http::problem_response(status::bad_request, std::string{"caf\xC3\xA9 \xFF"}).body.find("caf?? ?") !=
      std::string::npos
  );
  CHECK(http::problem_response(static_cast<status>(418), std::nullopt).body == R"({"status":418})");
}

TEST_CASE("ensure_ok: a response that is no error goes through, another is raised received", "[http][http_error]") {
  CHECK(http::ensure_ok(answered_with(status::created, "made")).body == "made");

  try {
    static_cast<void>(http::ensure_ok(answered_with(status::not_found, "missing")));
    FAIL("an error status is raised");
  } catch (http::http_error const &error) {
    CHECK(error.received());
    CHECK(error.status() == status::not_found);
    CHECK(std::string_view{error.what()} == "404 Not Found");
    REQUIRE(error.answer() != nullptr);
    CHECK(error.answer()->body == "missing");
  }
}

TEST_CASE("http_error: a middleware raising one is answered as a handler's", "[http][http_error]") {
  http::router router;

  router.get("/me", []() -> http::response { return http::response{}; });
  router.use([](http::request, http::next) -> task<http::response> {
    throw http::http_error{status::unauthorized, "expired session"};
  });

  auto const answer = dispatch(router, verb::get, "/me");

  CHECK(answer.status == status::unauthorized);
  CHECK(answer.body.find("expired session") != std::string::npos);
}

TEST_CASE("http_error: one received with a status that is no error still answers 502", "[http][http_error]") {
  http::router router;

  // As a client raises for a 200 whose body does not decode.
  router.get("/copy", []() -> http::response {
    throw http::http_error::received_from(answered_with(status::ok, "not what was asked"), "invalid body");
  });

  CHECK(dispatch(router, verb::get, "/copy").status == status::bad_gateway);
}

TEST_CASE("http_error: each middleware sees the one under it as a response", "[http][http_error]") {
  http::router router;
  std::string seen;

  router.get("/me", []() -> http::response { return http::response{}; });
  // The outer one, as a CORS middleware adding its field to whatever comes back.
  router.use([&seen](http::request message, http::next next) -> task<http::response> {
    auto answer = co_await next(std::move(message));

    seen = std::to_string(static_cast<int>(answer.status));
    answer.fields.add("Access-Control-Allow-Origin", "*");
    co_return answer;
  });
  router.use([](http::request, http::next) -> task<http::response> {
    throw http::http_error{status::unauthorized, "expired session"};
  });

  auto const answer = dispatch(router, verb::get, "/me");

  CHECK(answer.status == status::unauthorized);
  CHECK(seen == "401");
  CHECK(answer.fields.find("Access-Control-Allow-Origin") == "*");
}

TEST_CASE("http_error: a handler raising one before its task is made is answered", "[http][http_error]") {
  http::router router;

  router.add(verb::get, "/now", "now", [](http::request, http::route_match) -> task<http::response> {
    throw http::http_error{status::service_unavailable, "busy"};
  });

  CHECK(dispatch(router, verb::get, "/now").status == status::service_unavailable);
}
