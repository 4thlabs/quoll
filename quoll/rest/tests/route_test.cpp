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
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace route_test {

namespace http = quoll::http;
namespace rest = quoll::rest;

using quoll::async::task;

struct note {
  std::int64_t id = 0;
  std::string text;
};

struct new_note {
  std::string text;
};

namespace notes {

using list = rest::get<
    "/notes?contains={contains}&page={page}", std::vector<note>, std::optional<std::string>, std::optional<int>>;
using get = rest::get<"/notes/{id}", note, std::int64_t>;
using create = rest::post<"/notes", rest::created<note>, new_note>;
using remove = rest::del<"/notes/{id}", void, std::int64_t>;
using ping = rest::get<"/ping", rest::text, rest::operation_id<"notes.ping">>;

using routes = rest::group<list, get, create, remove, ping>;

} // namespace notes

using api = rest::api<"/api", notes::routes>;

// A service for some routes, rest::on for the others.
struct service {
  std::vector<note> operator()(notes::list, std::optional<std::string> contains, std::optional<int> page) const {
    return {{.id = page.value_or(0), .text = contains.value_or("-")}};
  }

  note operator()(notes::get, std::int64_t id) const { return {.id = id, .text = "found"}; }
};

http::router make_router() {
  http::router router;

  rest::mount<api>(
      router, service{}, rest::on<notes::create>([](new_note made) { return note{.id = 7, .text = made.text}; }),
      rest::on<notes::remove>([](std::int64_t) -> task<> { co_return; }),
      rest::on<notes::ping>([] { return std::string{"pong"}; })
  );
  return router;
}

template <class T>
T run(task<T> work) {
  return std::move(quoll::test::run(std::move(work)).value());
}

// Each route its own type, its arguments its parameters' types in order.
static_assert(std::same_as<api::route<notes::get>::arguments, std::tuple<std::int64_t>>);
static_assert(std::same_as<
              api::route<notes::list>::arguments, std::tuple<std::optional<std::string>, std::optional<int>>>);
static_assert(api::route<notes::get>::name == "GET /api/notes/{id}");
static_assert(api::route<notes::ping>::name == "notes.ping");
// Two routes of two paths are two types, whatever else they share.
static_assert(!std::same_as<notes::get, rest::get<"/tags/{id}", note, std::int64_t>>);

struct login {
  std::string user;
  std::string password;
};

struct token {
  std::string value;
};

// A header after its parameters, a form for its body, a group in a group, a result read leniently, the rest of a path.
namespace more {

using sign_in = rest::post<"/token", token, rest::header<"X-Device", std::optional<std::string>>, rest::form<login>>;
using file = rest::get<"/files/{rest:path}", rest::text, std::string>;
using lenient = rest::get<"/lenient", rest::lenient_json<token>>;

using routes = rest::group<rest::group<sign_in>, file, lenient>;

} // namespace more

using more_api = rest::api<"", more::routes>;

http::router make_more_router() {
  http::router router;

  rest::mount<more_api>(
      router, rest::on<more::sign_in>([](std::optional<std::string> device, login given) {
        return token{.value = given.user + "/" + device.value_or("-")};
      }),
      rest::on<more::file>([](std::string rest) { return rest; }),
      rest::on<more::lenient>([] { return token{.value = "kept"}; })
  );
  return router;
}

} // namespace route_test

using namespace route_test;

TEST_CASE("route: a header, a form, nested groups and the rest of a path", "[rest][route]") {
  auto const router = make_more_router();
  rest::client<more_api, http::local_transport> client{http::local_transport{router}};

  CHECK(
      run(client.call<more::sign_in>(std::optional<std::string>{"phone"}, login{.user = "ada", .password = "x"}))
          .value == "ada/phone"
  );
  CHECK(run(client.call<more::file>(std::string{"a/b.txt"})) == "a/b.txt");
  CHECK(run(client.call<more::lenient>()).value == "kept");
}

TEST_CASE("route: a contract of routes is served and called, by its path and its query", "[rest][route]") {
  auto const router = make_router();
  rest::client<api, http::local_transport> client{http::local_transport{router}};

  auto const found = run(client.call<notes::get>(3));

  CHECK(found.id == 3);
  CHECK(found.text == "found");

  auto const listed = run(client.call<notes::list>(std::optional<std::string>{"milk"}, std::optional<int>{2}));

  REQUIRE(listed.size() == 1);
  CHECK(listed.front().id == 2);
  CHECK(listed.front().text == "milk");
  CHECK(run(client.call<notes::list>(std::nullopt, std::nullopt)).front().text == "-");
  CHECK(run(client.call<notes::create>(new_note{.text = "tea"})).id == 7);
  CHECK(run(client.call<notes::ping>()) == "pong");
}

TEST_CASE("route: a route's status, a created one and one with no body", "[rest][route]") {
  auto const router = make_router();
  http::request created;

  created.method = http::verb::post;
  created.target = "/api/notes";
  created.body = R"({"text":"tea"})";
  created.fields.add("Content-Type", "application/json");

  CHECK(quoll::test::run(router.dispatch(std::move(created))).value().status == http::status::created);

  http::request removed;

  removed.method = http::verb::delete_;
  removed.target = "/api/notes/3";

  CHECK(quoll::test::run(router.dispatch(std::move(removed))).value().status == http::status::no_content);
}
