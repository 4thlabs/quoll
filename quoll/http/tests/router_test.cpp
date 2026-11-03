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
#include <quoll/http/local_transport.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace http = quoll::http;

using http::verb;
using quoll::async::task;

// Answers with what it was routed with: the route name, then each parameter, then the query, one per line.
task<http::response> echo(http::request, http::route_match match) {
  http::response answer;

  answer.body = std::string{match.name};

  for (auto const &param : match.params) {
    answer.body += "\n" + param;
  }

  answer.body += "\n?" + match.query;
  co_return answer;
}

http::router make_router() {
  http::router router;

  router.add(verb::get, "/users/{id:int64}", "users.get", echo);
  router.add(verb::delete_, "/users/{id}", "users.remove", echo);
  router.add(verb::get, "/users/me", "users.me", echo);
  router.add(verb::get, "/users", "users.list", echo);
  router.add(verb::get, "/files/{rest:path}", "files.get", echo);
  router.add(verb::get, "/", "root", echo);
  return router;
}

http::response dispatch(http::router const &router, verb method, std::string target) {
  http::request message;

  message.method = method;
  message.target = std::move(target);
  return quoll::test::run(router.dispatch(std::move(message))).value();
}

} // namespace

TEST_CASE("router: a route gets its name, its decoded parameters and the query", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/users/42").body == "users.get\n42\n?");
  REQUIRE(dispatch(router, verb::get, "/users/caf%C3%A9?limit=2&x=%20").body == "users.get\ncafé\n?limit=2&x=%20");
  REQUIRE(dispatch(router, verb::delete_, "/users/42").body == "users.remove\n42\n?");
  REQUIRE(dispatch(router, verb::get, "/users").body == "users.list\n?");
  REQUIRE(dispatch(router, verb::get, "/").body == "root\n?");
}

TEST_CASE("router: a literal segment beats a parameter, whatever the order they were added in", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/users/me").body == "users.me\n?");
  REQUIRE(dispatch(router, verb::get, "/users/mE").body == "users.get\nmE\n?");
}

TEST_CASE("router: {name:path} takes the rest of the path", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/files/a/b%20c").body == "files.get\na/b c\n?");
  REQUIRE(dispatch(router, verb::get, "/files/").body == "files.get\n\n?");
  REQUIRE(dispatch(router, verb::get, "/files").status == http::status::not_found);
}

TEST_CASE("router: no route is 404, another method 405 with Allow, a bad target 400", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/accounts").status == http::status::not_found);
  // Trailing slash: another path, and an empty segment is no {id}.
  REQUIRE(dispatch(router, verb::get, "/users/").status == http::status::not_found);

  auto const rejected = dispatch(router, verb::post, "/users/42");

  REQUIRE(rejected.status == http::status::method_not_allowed);
  REQUIRE(rejected.fields.find("Allow") == "GET, HEAD, DELETE");
  REQUIRE(dispatch(router, verb::get, "/users/%zz").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "*").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "ftp://example.com/users").status == http::status::bad_request);
}

TEST_CASE("router: a route a method already has, under other parameter names, throws", "[http][router]") {
  auto router = make_router();

  REQUIRE_THROWS_AS(router.add(verb::get, "/users/{uid}", "users.find", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/users/me", "users.self", echo), std::invalid_argument);
  // Same path, another method: a route of its own.
  REQUIRE_NOTHROW(router.add(verb::post, "/users/{uid}", "users.update", echo));
  REQUIRE_THROWS_AS(router.add(verb::get, "/users", "users.empty", http::router::handler{}), std::invalid_argument);
}

TEST_CASE("router: a {name} parameter beats the rest of the path", "[http][router]") {
  http::router router;

  router.add(verb::get, "/files/{rest:path}", "files.rest", echo);
  router.add(verb::get, "/files/{name}", "files.one", echo);

  REQUIRE(dispatch(router, verb::get, "/files/a").body == "files.one\na\n?");
  REQUIRE(dispatch(router, verb::get, "/files/a/b").body == "files.rest\na/b\n?");
}

TEST_CASE("router: what a handler throws comes out of the task", "[http][router]") {
  http::router router;

  // Throws when called, before it returns a task.
  router.add(verb::get, "/fails", "fails",
             [](http::request, http::route_match) -> task<http::response> { throw std::runtime_error{"refused"}; });

  http::request message;

  message.target = "/fails";

  auto work = router.dispatch(std::move(message));

  REQUIRE_THROWS_AS(quoll::test::run(std::move(work)), std::runtime_error);
}

TEST_CASE("router: an absolute-form target is routed by its path", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "http://example.com/users?limit=1").body == "users.list\n?limit=1");
  REQUIRE(dispatch(router, verb::get, "HTTPS://example.com:8443/users/42").body == "users.get\n42\n?");
  REQUIRE(dispatch(router, verb::get, "http://example.com").body == "root\n?");
  REQUIRE(dispatch(router, verb::get, "http://example.com?limit=1").body == "root\n?limit=1");
}

TEST_CASE("router: HEAD goes to the GET route", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::head, "/users/42").body == "users.get\n42\n?");
  // A method of its own when routed: DELETE has no GET counterpart to fall back on.
  REQUIRE(dispatch(router, verb::head, "/accounts").status == http::status::not_found);
}

TEST_CASE("router: dot segments are refused, an encoded slash stays in its segment", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/files/../etc/passwd").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/files/%2E%2E/%2e%2e/etc").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/files/./a").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/users/a%00b").status == http::status::bad_request);
  // In a {name} parameter, a character like any other.
  REQUIRE(dispatch(router, verb::get, "/users/a%2Fb").body == "users.get\na/b\n?");
  // In the rest of the path, it would read as a separator once joined.
  REQUIRE(dispatch(router, verb::get, "/files/a%2Fb").status == http::status::not_found);
}

TEST_CASE("router: a literal of the template is matched decoded", "[http][router]") {
  http::router router;

  router.add(verb::get, "/caf%C3%A9/a b", "cafe", echo);

  REQUIRE(dispatch(router, verb::get, "/caf%C3%A9/a%20b").body == "cafe\n?");
  REQUIRE(dispatch(router, verb::get, "/café/a%20b").body == "cafe\n?");
}

TEST_CASE("router: an invalid template throws", "[http][router]") {
  http::router router;

  REQUIRE_THROWS_AS(router.add(verb::get, "users", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/users/x{id}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/users/{id", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/users/{}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/users/{:int}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/x/{a{b}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/x/{a}b}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/x/{a-b}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/x/{id}/{id}", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/caf%zz", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "", "x", echo), std::invalid_argument);
  REQUIRE_THROWS_AS(router.add(verb::get, "/files/{rest:path}/meta", "x", echo), std::invalid_argument);
}

TEST_CASE("local_transport: a HEAD response has no body, as a connection writes it", "[http][router]") {
  auto const router = make_router();
  http::local_transport const transport{router};
  http::request message;

  message.method = verb::head;
  message.target = "/users/42";

  auto const answer = quoll::test::run(transport.send(std::move(message))).value();

  REQUIRE(answer.status == http::status::ok);
  REQUIRE(answer.body.empty());
}
