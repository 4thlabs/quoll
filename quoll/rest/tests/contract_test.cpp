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
#include <tuple>
#include <utility>
#include <vector>

// The bank API of docs/PLAN.md. Reflected types need external linkage (see json_codec).
namespace contract_test {

namespace rest = quoll::rest;

using quoll::http::status;
using quoll::http::verb;

struct user {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> email;
};

struct new_user {
  std::string name;
  std::string email;
};

struct user_filter {
  std::optional<int> limit;
  std::optional<std::string> name;
};

struct not_found {
  std::string detail;
};

struct card {
  std::int64_t id = 0;
};

struct credentials {
  std::string email;
  std::string password;
};

struct token {
  std::string value;
};

namespace auth {
using namespace quoll::rest;

using login = endpoint<"auth.login", verb::post, "/login", body<credentials>, returns<token>>;

using api = rest::api<"/auth", login>;
} // namespace auth

namespace users {
using namespace quoll::rest;

using list = endpoint<"users.list", verb::get, "/", query_params<user_filter>, returns<std::vector<user>>>;
using get = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>, returns<not_found, status::not_found>>;
using create = endpoint<"users.create", verb::post, "/", body<new_user>, returns<user, status::created>>;
using remove = endpoint<"users.remove", verb::delete_, "/{id:int64}">;

using api = rest::api<"/users", list, get, create, remove>;
} // namespace users

namespace cards {
using namespace quoll::rest;

using list = endpoint<"cards.list", verb::get, "/", query<"status", std::optional<std::string>>,
                      header<"X-Request-Id", std::string>, returns<std::vector<card>>>;
using remove = endpoint<"cards.remove", verb::delete_, "/{id:int64}">;

using api = rest::api<"/users/{user_id:int64}/cards", list, remove>;
} // namespace cards

using v1 = rest::api<
    "/api/v1", auth::api,
    rest::with<rest::security<rest::bearer_auth>, rest::query<"trace", std::optional<bool>>, users::api, cards::api>>;

} // namespace contract_test

namespace {

using namespace contract_test;

using quoll::rest::bearer_auth;
using quoll::rest::detail::type_list;

// Full paths: the prefixes, then the endpoint path ("/" is the prefix itself).
static_assert(v1::route<auth::login>::path == "/api/v1/auth/login");
static_assert(v1::route<users::list>::path == "/api/v1/users");
static_assert(v1::route<users::get>::path == "/api/v1/users/{id:int64}");
static_assert(v1::route<cards::remove>::path == "/api/v1/users/{user_id:int64}/cards/{id:int64}");

// Arguments: path parameters (prefixes first), the endpoint's argument parts in order, then the inherited ones.
static_assert(std::same_as<v1::route<auth::login>::arguments, std::tuple<credentials>>);
static_assert(std::same_as<v1::route<users::list>::arguments, std::tuple<user_filter, std::optional<bool>>>);
static_assert(std::same_as<v1::route<users::get>::arguments, std::tuple<std::int64_t, std::optional<bool>>>);
static_assert(std::same_as<v1::route<users::create>::arguments, std::tuple<new_user, std::optional<bool>>>);
static_assert(std::same_as<v1::route<cards::list>::arguments,
                           std::tuple<std::int64_t, std::optional<std::string>, std::string, std::optional<bool>>>);
static_assert(
    std::same_as<v1::route<cards::remove>::arguments, std::tuple<std::int64_t, std::int64_t, std::optional<bool>>>);

// Responses, in order; none means 204.
static_assert(v1::route<users::get>::responses::size == 2);
static_assert(v1::route<users::remove>::responses::size == 0);

// Security: inherited from with<> by the endpoints it wraps only.
static_assert(std::same_as<v1::route<users::get>::schemes, type_list<bearer_auth>>);
static_assert(std::same_as<v1::route<cards::remove>::schemes, type_list<bearer_auth>>);
static_assert(std::same_as<v1::route<auth::login>::schemes, type_list<>>);

// An endpoint is also routed by the api it is declared in, without the prefixes around it.
static_assert(users::api::route<users::get>::path == "/users/{id:int64}");
static_assert(std::same_as<users::api::route<users::get>::arguments, std::tuple<std::int64_t>>);

// What endpoint<> and route (with the parts inherited from with<>) reject, checked on the predicates of their
// static_asserts.
static_assert(quoll::rest::detail::count_bodies(
                  type_list<quoll::rest::body<new_user>, quoll::rest::query<"q", int>, quoll::rest::body<user>>{}) ==
              2);
static_assert(!quoll::rest::detail::distinct_statuses(
    type_list<quoll::rest::returns<user>, quoll::rest::returns<not_found, status::not_found>,
              quoll::rest::returns<card, status::not_found>>{}));
static_assert(quoll::rest::detail::distinct_statuses(v1::route<users::get>::parts{}));

// What api<> rejects, checked on the predicates its static_asserts use.
using clash = quoll::rest::endpoint<"users.get", verb::get, "/other">;
using same_route = quoll::rest::endpoint<"users.find", verb::get, "/{uid:int64}">;

static_assert(quoll::rest::detail::distinct_names(v1::routes{}));
static_assert(quoll::rest::detail::distinct_operations(v1::routes{}));
static_assert(!quoll::rest::detail::distinct_names(
    type_list<users::api::route<users::get>, quoll::rest::api<"", clash>::route<clash>>{}));
static_assert(!quoll::rest::detail::distinct_operations(
    type_list<users::api::route<users::get>, quoll::rest::api<"/users", same_route>::route<same_route>>{}));

} // namespace

TEST_CASE("rest contract: endpoints are routed under their prefixes", "[rest][contract]") {
  REQUIRE(v1::route<cards::remove>::path == "/api/v1/users/{user_id:int64}/cards/{id:int64}");
  REQUIRE(v1::route<users::list>::endpoint::name == "users.list");
  REQUIRE(v1::route<users::create>::endpoint::method == verb::post);
  REQUIRE(v1::prefix == "/api/v1");
}

namespace contract_test_routes {

using namespace quoll::rest;
using quoll::http::status;
using quoll::http::verb;

// A parameter and the rest of a path, in one place: the router tells them apart, and so does the api.
using one = endpoint<"one", verb::get, "/x/{a}", returns<std::string, status::ok, text_codec>>;
using rest_of = endpoint<"rest_of", verb::get, "/x/{b:path}", returns<std::string, status::ok, text_codec>>;
// The same status twice is refused; two responses of one type tell theirs with respond<>.
using made = endpoint<"made", verb::post, "/made", body<std::string, text_codec>,
                      returns<std::string, status::ok, text_codec>, returns<std::string, status::created, text_codec>>;
// A first response with no body.
using accepted = endpoint<"accepted", verb::post, "/accepted", returns<void, status::accepted>>;
// A prefix with a parameter: its argument comes first.
using owned = endpoint<"owned", verb::get, "/items/{item:int}", returns<std::string, status::ok, text_codec>>;

using api = quoll::rest::api<"", one, rest_of, made, accepted, quoll::rest::api<"/orgs/{org}", owned>>;

struct service {
  [[nodiscard]] std::string operator()(one, std::string a) const { return "one " + a; }

  [[nodiscard]] std::string operator()(rest_of, std::string b) const { return "rest " + b; }

  [[nodiscard]] reply<made> operator()(made, std::string text) const {
    if (text == "new") {
      return respond<status::created>(std::string{"created"});
    }

    return respond<status::ok>(std::string{"kept"});
  }

  void operator()(accepted) const {}

  [[nodiscard]] std::string operator()(owned, std::string org, int item) const {
    return org + "/" + std::to_string(item);
  }
};

} // namespace contract_test_routes

TEST_CASE("contract: what users write runs, through mount and a client", "[rest][contract]") {
  namespace routes = contract_test_routes;

  quoll::http::router router;

  quoll::rest::mount<routes::api>(router, routes::service{});

  quoll::rest::client<routes::api, quoll::http::local_transport> const client{quoll::http::local_transport{router}};
  auto const run = [](auto work) { return std::move(quoll::test::run(std::move(work)).value()); };

  REQUIRE(run(client.call<routes::one>("1")) == "one 1");
  REQUIRE(run(client.call<routes::rest_of>("1/2")) == "rest 1/2");

  auto const created = run(client.try_call<routes::made>("new"));

  REQUIRE(created->holds<quoll::http::status::created>());
  REQUIRE(created->get<quoll::http::status::created>() == "created");
  REQUIRE(run(client.call<routes::made>("old")) == "kept");
  REQUIRE(run(client.try_call<routes::accepted>())->holds<quoll::http::status::accepted>());
  REQUIRE(run(client.call<routes::owned>("acme", 7)) == "acme/7");
}
