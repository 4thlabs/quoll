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
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Reflected types need external linkage (see json_codec).
namespace client_test {

namespace rest = quoll::rest;

using quoll::async::task;
using quoll::http::status;
using quoll::http::verb;

struct user {
  std::int64_t id = 0;
  std::string name;
};

struct new_user {
  std::string name;
};

struct user_filter {
  std::optional<int> limit;
  std::optional<std::string> name;
  std::vector<std::string> tag;
};

struct not_found {
  std::string detail;
};

namespace users {
using namespace quoll::rest;

using list = endpoint<"users.list", verb::get, "/", query_params<user_filter>, returns<std::vector<user>>>;
using get = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>, returns<not_found, status::not_found>>;
using create = endpoint<"users.create", verb::post, "/", body<new_user>, returns<user, status::created>>;
using remove = endpoint<"users.remove", verb::delete_, "/{id:int64}">;

using api = rest::api<"/users", list, get, create, remove>;
} // namespace users

namespace files {
using namespace quoll::rest;

// Echo what reached the server: parameters with characters a URL escapes.
using one = endpoint<"files.one", verb::get, "/one/{name}", query<"q", std::optional<std::string>>,
                     header<"X-Tags", std::vector<std::string>>, returns<std::string, status::ok, text_codec>>;
using rest_of = endpoint<"files.rest", verb::get, "/rest/{rest:path}", returns<std::string, status::ok, text_codec>>;

// Required parameters of a type that is no class.
using paged = endpoint<"files.paged", verb::get, "/paged", query<"page", std::int64_t>, query<"all", bool>,
                       header<"X-Limit", std::int64_t>, returns<std::string, status::ok, text_codec>>;

using api = rest::api<"/files", one, rest_of, paged>;
} // namespace files

namespace keys {
using namespace quoll::rest;

using ping = endpoint<"keys.ping", verb::get, "/ping", security<api_key_query<"key">>,
                      returns<std::string, status::ok, text_codec>>;

using api = rest::api<"/keys", ping>;
} // namespace keys

using v1 = rest::api<"/api/v1", keys::api, files::api, rest::with<rest::security<rest::bearer_auth>, users::api>>;

struct users_service {
  task<std::vector<user>> operator()(users::list, user_filter filter) const {
    std::string described = filter.limit ? std::to_string(*filter.limit) : "-";

    described += "|" + filter.name.value_or("-");

    for (auto const &tag : filter.tag) {
      described += "|" + tag;
    }

    co_return std::vector<user>{{.id = 1, .name = described}};
  }

  task<rest::reply<users::get>> operator()(users::get, std::int64_t id) const {
    if (id == 42) {
      co_return user{.id = 42, .name = "Ada"};
    }

    co_return not_found{"no user " + std::to_string(id)};
  }

  task<user> operator()(users::create, new_user created) const {
    co_return user{.id = 7, .name = std::move(created.name)};
  }

  task<> operator()(users::remove, std::int64_t) const { co_return; }
};

struct files_service {
  std::string operator()(files::paged, std::int64_t page, bool all, std::int64_t limit) const {
    return std::to_string(page) + (all ? " all " : " some ") + std::to_string(limit);
  }

  std::string operator()(files::one, std::string name, std::optional<std::string> query,
                         std::vector<std::string> tags) const {
    std::string echoed = name + "|" + query.value_or("-");

    for (auto const &tag : tags) {
      echoed += "|" + tag;
    }

    return echoed;
  }

  std::string operator()(files::rest_of, std::string rest) const { return rest; }
};

struct keys_service {
  std::string operator()(keys::ping) const { return "pong"; }
};

} // namespace client_test

namespace {

using namespace client_test;

namespace http = quoll::http;

using api_client = rest::client<v1, http::local_transport>;

http::router make_router() {
  http::router router;

  rest::mount<v1>(
      router, users_service{}, files_service{}, keys_service{},
      rest::validators(
          [](rest::credential<rest::bearer_auth> const &given) { return given.value == "valid"; },
          [](rest::credential<rest::api_key_query<"key">> const &given) { return given.value == "k e/y"; }));
  return router;
}

api_client make_client(http::router const &router) {
  api_client client{http::local_transport{router}};

  client.credentials<rest::bearer_auth>("valid");
  return client;
}

template <class T>
T run(task<T> work) {
  return std::move(quoll::test::run(std::move(work)).value());
}

// Whether a task<void> completed: quoll::test::run takes tasks with a value.
task<bool> completes(task<> work) {
  co_await std::move(work);
  co_return true;
}

// A transport answering the same response to every request.
struct canned_transport {
  http::response answer;

  [[nodiscard]] task<http::response> send(http::request) const { co_return answer; }
};

} // namespace

TEST_CASE("client: call returns the first declared response", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);
  auto const ada = run(client.call<users::get>(42));

  REQUIRE(ada.id == 42);
  REQUIRE(ada.name == "Ada");
  REQUIRE(run(client.call<users::create>({.name = "Grace"})).id == 7);
  // No returns<>: a task<void>, done on the 204.
  REQUIRE(run(completes(client.call<users::remove>(42))));
}

TEST_CASE("client: braced arguments fill query_params, encoded and decoded", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);
  auto const listed = run(client.call<users::list>({.limit = 2, .name = "A da&=", .tag = {"a", "b+c"}}));

  REQUIRE(listed.front().name == "2|A da&=|a|b+c");
  REQUIRE(run(client.call<users::list>({})).front().name == "-|-");
}

TEST_CASE("client: required parameters of a scalar type reach the server", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);

  REQUIRE(run(client.call<files::paged>(3, true, 20)) == "3 all 20");
  REQUIRE(run(client.call<files::paged>(-1, false, 0)) == "-1 some 0");
}

TEST_CASE("client: a middleware of the router wraps REST routes, through a local transport too", "[rest][client]") {
  auto router = make_router();
  int passed = 0;

  router.use([&passed](http::request message, http::next next) -> task<http::response> {
    if (message.method == verb::post) {
      http::response refused;

      refused.status = status::forbidden;
      co_return refused;
    }

    auto answer = co_await next(std::move(message));

    ++passed;
    co_return answer;
  });

  auto const client = make_client(router);

  REQUIRE(run(client.call<users::list>({})).front().name == "-|-");
  REQUIRE(passed == 1);

  try {
    static_cast<void>(run(client.call<users::create>({.name = "Ada"})));
    FAIL("a refused call throws");
  } catch (rest::call_error const &failure) {
    REQUIRE(failure.status() == status::forbidden);
  }

  REQUIRE(passed == 1);
}

TEST_CASE("client: path parameters, query and headers are escaped", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);

  REQUIRE(run(client.call<files::one>("a b/c?d", "x y", {"t1", "t 2"})) == "a b/c?d|x y|t1|t 2");
  REQUIRE(run(client.call<files::one>("é", std::nullopt, {})) == "é|-");
  // The rest of a path keeps its slashes.
  REQUIRE(run(client.call<files::rest_of>("dir/sub/a b")) == "dir/sub/a b");
}

TEST_CASE("client: try_call hands over any declared response", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);
  auto const missing = run(client.try_call<users::get>(7));

  REQUIRE(missing.has_value());
  REQUIRE(missing->status() == http::status::not_found);
  REQUIRE(missing->get<http::status::not_found>().detail == "no user 7");

  auto const found = run(client.try_call<users::get>(42));

  REQUIRE(found->holds<http::status::ok>());
  REQUIRE(found->get<http::status::ok>().name == "Ada");
  REQUIRE(run(client.try_call<users::remove>(42)).has_value());
}

TEST_CASE("client: call throws call_error for any other outcome", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);

  try {
    (void)run(client.call<users::get>(7));
    FAIL("call returned");
  } catch (rest::call_error const &error) {
    REQUIRE(error.status() == http::status::not_found);
    REQUIRE(error.failure().reason == "declared response 404: read it with try_call");
  }
}

TEST_CASE("client: credentials are sent to the endpoints that require them", "[rest][client]") {
  auto const router = make_router();
  api_client anonymous{http::local_transport{router}};
  auto const refused = run(anonymous.try_call<users::get>(42));

  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().response.status == http::status::unauthorized);
  REQUIRE(refused.error().reason == "undeclared status 401");

  try {
    (void)run(anonymous.call<users::get>(42));
    FAIL("call returned");
  } catch (rest::call_error const &error) {
    // The detail of the server's problem.
    REQUIRE(error.status() == http::status::unauthorized);
    REQUIRE(std::string{error.what()} == "missing credentials");
  }

  anonymous.credentials<rest::api_key_query<"key">>("k e/y");

  REQUIRE(run(anonymous.call<keys::ping>()) == "pong");
}

TEST_CASE("client: a declared response whose body does not decode is unexpected", "[rest][client]") {
  http::response garbage;

  garbage.status = http::status::ok;
  garbage.body = "{not json";

  rest::client<v1, canned_transport> const client{canned_transport{garbage}};
  auto const received = run(client.try_call<users::get>(42));

  REQUIRE_FALSE(received.has_value());
  REQUIRE(received.error().reason.starts_with("invalid body: "));
  REQUIRE_THROWS_AS(run(client.call<users::get>(42)), rest::call_error);
}

TEST_CASE("client: a path parameter must stay a segment where the template puts it", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);

  // A URL parser resolves dot segments: the request would reach another endpoint, with the credentials.
  REQUIRE_THROWS_AS((void)client.call<files::one>("..", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>(".", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>(".\t.", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>("", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("a/../../admin"), std::invalid_argument);
  // Encoding hides the slash from the client, not from a proxy that decodes it, nor from Quoll's router.
  REQUIRE_THROWS_AS((void)client.call<files::one>("../admin", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>("a\\..", std::nullopt, {}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("a\\..\\admin"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("./x"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("a/."), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("..\\x"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>(std::string{"a\0b", 3}), std::invalid_argument);
  // The rest of a path goes out with its '/': under a template at the root, `//host/x` reads as another server.
  REQUIRE_THROWS_AS((void)client.call<files::rest_of>("/host/x"), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>(std::string{"a\0b", 3}, std::nullopt, {}), std::invalid_argument);
  // Dots inside a name are fine.
  REQUIRE(run(client.call<files::one>("..a", std::nullopt, {})) == "..a|-");
  // The rest of a path may be empty, where a single segment may not.
  REQUIRE(run(client.call<files::rest_of>("")).empty());
  // A slash in a parameter that is no dot segment goes encoded, and comes back as it was.
  REQUIRE(run(client.call<files::one>("a/b", std::nullopt, {})) == "a/b|-");
}

TEST_CASE("client: a header value the server would read differently is refused", "[rest][client]") {
  auto const router = make_router();
  auto const client = make_client(router);

  REQUIRE_THROWS_AS((void)client.call<files::one>("x", std::nullopt, {"a,b"}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>("x", std::nullopt, {" c"}), std::invalid_argument);
  REQUIRE_THROWS_AS((void)client.call<files::one>("x", std::nullopt, {""}), std::invalid_argument);
}

TEST_CASE("client: the detail of a problem, whatever the case of its media type", "[rest][client]") {
  http::response gone;

  gone.status = http::status::not_found;
  gone.fields.add("Content-Type", "Application/Problem+JSON");
  gone.body = R"({"detail":"gone for good"})";

  rest::client<v1, canned_transport> const client{canned_transport{gone}};

  try {
    run(completes(client.call<users::remove>(42)));
    FAIL("call returned");
  } catch (rest::call_error const &error) {
    REQUIRE(std::string{error.what()} == "gone for good");
  }
}

TEST_CASE("client: a response declared by a with<> is not held by reply<Endpoint>, and says so", "[rest][client]") {
  using wrapped = rest::api<"/api/v1", rest::with<rest::returns<not_found, status::conflict>, users::api>>;

  http::response missing;

  missing.status = http::status::conflict;
  missing.body = R"({"detail":"no such user"})";

  rest::client<wrapped, canned_transport> const client{canned_transport{missing}};
  auto const removed = run(client.try_call<users::remove>(42));

  REQUIRE_FALSE(removed.has_value());
  REQUIRE(removed.error().reason ==
          "status 409 is declared by a with<> around the endpoint: reply<Endpoint> does not hold it");
}
