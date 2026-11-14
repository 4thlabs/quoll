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

struct note_filter {
  std::optional<std::string> contains;
  std::optional<int> limit;
};

namespace notes {

using list = rest::get<
    "/notes?contains={contains}&page={page}", std::vector<note>, std::optional<std::string>, std::optional<int>>;
using get = rest::get<"/notes/{id}", note, std::int64_t>;
using create = rest::post<"/notes", rest::created<note>, new_note>;
using remove = rest::del<"/notes/{id}", void, std::int64_t>;
using ping = rest::get<"/ping", rest::text, rest::operation_id<"notes.ping">>;
using search = rest::get<"/notes/search", std::vector<note>, rest::query_params<note_filter>>;

using routes = rest::group<list, get, create, remove, ping, search>;

} // namespace notes

using api = rest::api<"/api", notes::routes>;

// A service for some routes, a lambda for each of the others, its first parameter the route it serves.
struct service {
  std::vector<note> operator()(notes::list, std::optional<std::string> contains, std::optional<int> page) const {
    return {{.id = page.value_or(0), .text = contains.value_or("-")}};
  }

  note operator()(notes::get, std::int64_t id) const { return {.id = id, .text = "found"}; }

  std::vector<note> operator()(notes::search, note_filter filter) const {
    return {{.id = filter.limit.value_or(0), .text = filter.contains.value_or("-")}};
  }
};

http::router make_router() {
  http::router router;

  rest::mount<api>(
      router, service{}, [](notes::create, new_note made) { return note{.id = 7, .text = made.text}; },
      [](notes::remove, std::int64_t) -> task<> { co_return; }, [](notes::ping) { return std::string{"pong"}; }
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
static_assert(quoll::rest::detail::Operation<notes::get>);
static_assert(!quoll::rest::detail::Operation<note>);
// Two routes of two paths are two types, whatever else they share.
static_assert(!std::same_as<notes::get, rest::get<"/tags/{id}", note, std::int64_t>>);

struct login {
  std::string user;
  std::string password;
};

struct token {
  std::string value;
};

// A header after its parameters, a form for its body, a group in a group, the rest of a path.
namespace more {

using sign_in = rest::post<"/token", token, rest::header<"X-Device", std::optional<std::string>>, rest::form<login>>;
using file = rest::get<"/files/{rest:path}", rest::text, std::string>;

using routes = rest::group<rest::group<sign_in>, file>;

} // namespace more

using more_api = rest::api<"", more::routes>;

http::router make_more_router() {
  http::router router;

  // A service defined once, its handlers each taking its route first.
  auto const handlers = rest::service(
      [](more::sign_in, std::optional<std::string> device, login given) {
        return token{.value = given.user + "/" + device.value_or("-")};
      },
      [](more::file, std::string rest) { return rest; }
  );

  rest::mount<more_api>(router, handlers);
  return router;
}

// A parameter routes share, in each one's path; the other methods, statuses and formats.
namespace repos {

using update = rest::put<"/repos/{owner}/notes/{id}", note, std::string, std::int64_t, new_note>;
using touch =
    rest::patch<"/repos/{owner}/notes/{id}", rest::accepted<note>, std::string, std::int64_t, rest::json<new_note>>;
using exists = rest::head<"/repos/{owner}/notes/{id}", void, std::string, std::int64_t>;
using allowed = rest::options<"/repos/{owner}/notes", void, std::string>;
using raw = rest::get<"/repos/{owner}/raw", rest::bytes, std::string>;

} // namespace repos

using repos_api =
    rest::api<"/api", rest::group<repos::update, repos::touch, repos::exists, repos::allowed, repos::raw>>;

// The path's parameters in order, then the body.
static_assert(std::same_as<
              repos_api::route<repos::update>::arguments, std::tuple<std::string, std::int64_t, new_note>>);

http::router make_repos_router() {
  http::router router;

  rest::mount<repos_api>(
      router, rest::service(
                  [](repos::update, std::string owner, std::int64_t id, new_note changed) {
                    return note{.id = id, .text = owner + ":" + changed.text};
                  },
                  [](repos::touch, std::string owner, std::int64_t id, new_note changed) {
                    return note{.id = id, .text = owner + "~" + changed.text};
                  },
                  [](repos::exists, std::string, std::int64_t) {}, [](repos::allowed, std::string) {},
                  [](repos::raw, std::string) { return std::vector<std::byte>{std::byte{1}, std::byte{2}}; }
              )
  );
  return router;
}

// The status `router` answers a request of `method` to `target`, with a JSON `body` if any.
http::status status_of(http::router const &router, http::verb method, std::string target, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);

  if (!body.empty()) {
    request.body = std::move(body);
    request.fields.add("Content-Type", "application/json");
  }

  return quoll::test::run(router.dispatch(std::move(request))).value().status;
}

// A group's security for its routes, and a query parameter that repeats.
namespace locked {

using tagged = rest::get<"/tagged?tag={tag}", std::vector<std::string>, std::vector<std::string>>;
using open = rest::get<"/open", rest::text>;

using routes = rest::group<rest::group<rest::security<rest::bearer_auth>, tagged>, open>;

} // namespace locked

using locked_api = rest::api<"", locked::routes>;

http::router make_locked_router() {
  http::router router;

  rest::mount<locked_api>(
      router,
      rest::service(
          [](locked::tagged, std::vector<std::string> tags) { return tags; },
          [](locked::open) { return std::string{"open"}; }
      ),
      rest::validators([](rest::credential<rest::bearer_auth> const &given) { return given.value == "valid"; })
  );
  return router;
}

// A result read leniently: the server's has a field the client's type does not know.
struct wide_token {
  std::string value;
  int extra = 0;
};

namespace lenient {

using served = rest::get<"/token", wide_token>;
using strict = rest::get<"/token", token>;
using read = rest::get<"/token", rest::lenient_json<token>>;

} // namespace lenient

http::router make_lenient_router() {
  http::router router;

  rest::mount<rest::api<"", rest::group<lenient::served>>>(router, rest::service([](lenient::served) {
                                                             return wide_token{.value = "kept", .extra = 1};
                                                           }));
  return router;
}

// A server answering every request with the same status and body, as a third-party API may.
struct canned_transport {
  http::status status = http::status::ok;
  std::string body;

  [[nodiscard]] task<http::response> send(http::request) const { return answering(status, body); }

  [[nodiscard]] static task<http::response> answering(http::status status, std::string body) {
    http::response answer;

    answer.status = status;
    answer.body = std::move(body);
    co_return answer;
  }
};

namespace third_party {

using exists = rest::head<"/notes/{id}", void, std::int64_t>;
using forget = rest::del<"/notes/{id}", void, std::int64_t>;
using create = rest::post<"/notes", rest::created<note>, new_note>;

using api = rest::api<"", rest::group<exists, forget, create>>;

} // namespace third_party

} // namespace route_test

using namespace route_test;

TEST_CASE("route: any 2xx is a route's success, its body read for a result, none for a void one", "[rest][route]") {
  rest::client<third_party::api, canned_transport> const ok{canned_transport{http::status::ok, R"({"id":4,"text":"x"})"}
  };

  // A HEAD answered 200, as GET would be; a DELETE answered 200 with a body it has no use for.
  CHECK(quoll::test::run(ok.try_call<third_party::exists>(4)).value().has_value());
  CHECK(quoll::test::run(ok.try_call<third_party::forget>(4)).value().has_value());
  // A created note answered 200 rather than 201: still its value.
  CHECK(run(ok.call<third_party::create>(new_note{.text = "x"})).id == 4);

  rest::client<third_party::api, canned_transport> const moved{canned_transport{http::status::found, ""}};
  auto const refused = quoll::test::run(moved.try_call<third_party::exists>(4)).value();

  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().status() == http::status::found);
}

TEST_CASE("route: a result read leniently skips the fields its type does not know", "[rest][route]") {
  auto const router = make_lenient_router();
  // Two apis, as the two routes share a method and a path.
  rest::client<rest::api<"", rest::group<lenient::read>>, http::local_transport> lenient_client{
      http::local_transport{router}
  };
  rest::client<rest::api<"", rest::group<lenient::strict>>, http::local_transport> strict_client{
      http::local_transport{router}
  };

  CHECK(run(lenient_client.call<lenient::read>()).value == "kept");
  // The same body read strictly is refused: the lenient read is what skipped the field.
  CHECK_THROWS(run(strict_client.call<lenient::strict>()));
}

TEST_CASE("route: a group's security, and a query parameter that repeats", "[rest][route]") {
  auto const router = make_locked_router();
  rest::client<locked_api, http::local_transport> client{http::local_transport{router}};

  CHECK(run(client.call<locked::open>()) == "open");
  CHECK(status_of(router, http::verb::get, "/tagged?tag=a") == http::status::unauthorized);

  client.credentials<rest::bearer_auth>("valid");

  CHECK(run(client.call<locked::tagged>(std::vector<std::string>{"a", "b"})) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("route: a parameter routes share, put, patch, head, options, accepted and bytes", "[rest][route]") {
  auto const router = make_repos_router();
  rest::client<repos_api, http::local_transport> client{http::local_transport{router}};

  CHECK(run(client.call<repos::update>(std::string{"ada"}, 3, new_note{.text = "tea"})).text == "ada:tea");
  CHECK(run(client.call<repos::touch>(std::string{"ada"}, 4, new_note{.text = "milk"})).text == "ada~milk");
  CHECK(run(client.call<repos::raw>(std::string{"ada"})).size() == 2);

  CHECK(status_of(router, http::verb::patch, "/api/repos/ada/notes/4", R"({"text":"x"})") == http::status::accepted);
  CHECK(status_of(router, http::verb::head, "/api/repos/ada/notes/4") == http::status::no_content);
  CHECK(status_of(router, http::verb::options, "/api/repos/ada/notes") == http::status::no_content);
}

TEST_CASE("route: a header, a form, nested groups and the rest of a path", "[rest][route]") {
  auto const router = make_more_router();
  rest::client<more_api, http::local_transport> client{http::local_transport{router}};

  CHECK(
      run(client.call<more::sign_in>(std::optional<std::string>{"phone"}, login{.user = "ada", .password = "x"}))
          .value == "ada/phone"
  );
  CHECK(run(client.call<more::file>(std::string{"a/b.txt"})) == "a/b.txt");
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

  auto const searched = run(client.call<notes::search>({.contains = "tea", .limit = 5}));

  REQUIRE(searched.size() == 1);
  CHECK(searched.front().id == 5);
  CHECK(searched.front().text == "tea");
}

TEST_CASE("route: a route's status, a created one and one with no body", "[rest][route]") {
  auto const router = make_router();

  CHECK(status_of(router, http::verb::post, "/api/notes", R"({"text":"tea"})") == http::status::created);
  CHECK(status_of(router, http::verb::delete_, "/api/notes/3") == http::status::no_content);
}
