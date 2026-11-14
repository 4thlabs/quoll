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

#include <quoll/http/http_error.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The bank API of docs/PLAN.md. Reflected types need external linkage (see json_codec).
namespace contract_test {

namespace rest = quoll::rest;

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

using login = rest::post<"/auth/login", token, credentials>;

} // namespace auth

namespace users {

using list = rest::get<"/users", std::vector<user>, rest::query_params<user_filter>>;
using get = rest::get<"/users/{id}", user, std::int64_t, rest::operation_id<"users.get">>;
using create = rest::post<"/users", rest::created<user>, new_user>;
using forget = rest::del<"/users/{id}", void, std::int64_t>;

using routes = rest::group<list, get, create, forget>;

} // namespace users

namespace cards {

using list = rest::get<
    "/users/{user_id}/cards?status={status}", std::vector<card>, std::int64_t, std::optional<std::string>,
    rest::header<"X-Request-Id", std::string>>;
using forget = rest::del<"/users/{user_id}/cards/{id}", void, std::int64_t, std::int64_t>;

using routes = rest::group<list, forget>;

} // namespace cards

using v1 = rest::api<
    "/api/v1", auth::login,
    rest::group<
        rest::security<rest::bearer_auth>, rest::header<"X-Trace", std::optional<bool>>, users::routes, cards::routes>>;

} // namespace contract_test

namespace {

using namespace contract_test;

using quoll::rest::bearer_auth;
using quoll::rest::detail::type_list;

// Full paths: the api's prefix, then the route's.
static_assert(v1::route<auth::login>::path == "/api/v1/auth/login");
static_assert(v1::route<users::list>::path == "/api/v1/users");
static_assert(v1::route<users::get>::path == "/api/v1/users/{id}");
static_assert(v1::route<cards::forget>::path == "/api/v1/users/{user_id}/cards/{id}");

// Arguments: the path's and the query's parameters, the route's own parts in order, then what its groups give.
static_assert(std::same_as<v1::route<auth::login>::arguments, std::tuple<credentials>>);
static_assert(std::same_as<v1::route<users::list>::arguments, std::tuple<user_filter, std::optional<bool>>>);
static_assert(std::same_as<v1::route<users::get>::arguments, std::tuple<std::int64_t, std::optional<bool>>>);
static_assert(std::same_as<v1::route<users::create>::arguments, std::tuple<new_user, std::optional<bool>>>);
static_assert(std::same_as<
              v1::route<cards::list>::arguments,
              std::tuple<std::int64_t, std::optional<std::string>, std::string, std::optional<bool>>>);
static_assert(std::same_as<
              v1::route<cards::forget>::arguments, std::tuple<std::int64_t, std::int64_t, std::optional<bool>>>);

// Its result: one, or none for void, answered 204.
static_assert(v1::route<users::get>::responses::size == 1);
static_assert(v1::route<users::forget>::responses::size == 0);

// Security: given by a group to the routes it holds only.
static_assert(std::same_as<v1::route<users::get>::schemes, type_list<bearer_auth>>);
static_assert(std::same_as<v1::route<cards::forget>::schemes, type_list<bearer_auth>>);
static_assert(std::same_as<v1::route<auth::login>::schemes, type_list<>>);

// A route's name: its operation_id<>, else its method and whole path.
static_assert(v1::route<users::get>::name == "users.get");
static_assert(v1::route<users::list>::name == "GET /api/v1/users");

// What api<> rejects, checked on the predicates its static_asserts use.
using clash = quoll::rest::get<"/other", void, quoll::rest::operation_id<"users.get">>;
using same_route = quoll::rest::get<"/users/{uid}", void, std::int64_t>;

static_assert(quoll::rest::detail::distinct_names(v1::routes{}));
static_assert(quoll::rest::detail::distinct_operations(v1::routes{}));
static_assert(!quoll::rest::detail::distinct_names(
    type_list<v1::route<users::get>, quoll::rest::api<"/api/v1", clash>::route<clash>>{}
));
static_assert(!quoll::rest::detail::distinct_operations(
    type_list<v1::route<users::get>, quoll::rest::api<"/api/v1", same_route>::route<same_route>>{}
));

} // namespace

TEST_CASE("rest contract: routes are served under the api's prefix", "[rest][contract]") {
  REQUIRE(v1::route<cards::forget>::path == "/api/v1/users/{user_id}/cards/{id}");
  REQUIRE(v1::route<users::create>::method == verb::post);
  REQUIRE(v1::prefix == "/api/v1");
}

namespace contract_test_routes {

namespace rest = quoll::rest;

// A codec whose default constructor is no constant: one is built where it is used, at run time, on both sides.
class marked_codec {
public:
  static constexpr std::string_view content_type = "text/x-marked";

  // NOLINTNEXTLINE(modernize-use-equals-default): not constexpr, which is the point.
  marked_codec() : mark_(">") {}

  [[nodiscard]] std::string encode(std::string const &value) const { return mark_ + value; }

  template <class T>
    requires std::same_as<T, std::string>
  [[nodiscard]] std::expected<T, rest::decode_error> decode(std::string_view body) const {
    if (!body.starts_with(mark_)) {
      return std::unexpected{rest::decode_error{"not marked"}};
    }

    return std::string{body.substr(mark_.size())};
  }

private:
  std::string mark_;
};

static_assert(rest::Codec<marked_codec, std::string>);

// A parameter and the rest of a path, in one place: the router tells them apart, and so does the api.
using one = rest::get<"/x/{a}", rest::text, std::string>;
using rest_of = rest::get<"/x/{b:path}", rest::text, std::string>;
// A result made, answered 201.
using made = rest::post<"/made", rest::created<rest::text>, rest::text>;
// A result with no body, answered 202.
using queued = rest::post<"/queued", rest::accepted<void>>;
// A parameter several routes share, in each one's path.
using owned = rest::get<"/orgs/{org}/items/{item}", rest::text, std::string, int>;
// Bodies both ways through that codec.
using marked =
    rest::post<"/marked", rest::encoded<std::string, marked_codec>, rest::encoded<std::string, marked_codec>>;

using api = rest::api<"", rest::group<one, rest_of, made, queued, owned, marked>>;

struct service {
  [[nodiscard]] std::string operator()(one, std::string a) const { return "one " + a; }

  [[nodiscard]] std::string operator()(rest_of, std::string b) const { return "rest " + b; }

  [[nodiscard]] std::string operator()(made, std::string text) const { return "made " + text; }

  void operator()(queued) const {}

  [[nodiscard]] std::string operator()(marked, std::string text) const { return text + "!"; }

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
  REQUIRE(run(client.call<routes::made>("tea")) == "made tea");

  auto const made = run(client.try_call<routes::made>("milk"));

  REQUIRE(made.has_value());
  REQUIRE(*made == "made milk");
  REQUIRE(run(client.try_call<routes::queued>()).has_value());
  REQUIRE(run(client.call<routes::owned>("acme", 7)) == "acme/7");
  // Encoded by the client, decoded and encoded by the server, decoded by the client: each with a codec built then.
  REQUIRE(run(client.call<routes::marked>("hi")) == "hi!");
}

TEST_CASE("contract: try_call gives another status as the http_error received", "[rest][contract]") {
  namespace routes = contract_test_routes;

  quoll::http::router router;

  quoll::rest::mount<routes::api>(router, routes::service{});

  // A route the server does not have: its 404 comes back as an error, its status and body as received.
  using missing = quoll::rest::get<"/missing", quoll::rest::text>;

  quoll::rest::client<quoll::rest::api<"", missing>, quoll::http::local_transport> const client{
      quoll::http::local_transport{router}
  };
  auto const answer = std::move(quoll::test::run(client.try_call<missing>()).value());

  REQUIRE_FALSE(answer.has_value());
  REQUIRE(answer.error().status() == quoll::http::status::not_found);
  REQUIRE(answer.error().received());
  REQUIRE_THROWS_AS(quoll::test::run(client.call<missing>()), quoll::http::http_error);
}

namespace contract_test_size {

namespace rest = quoll::rest;

// Each path is parsed once to tell operations apart: comparing every pair ran out of Clang's constexpr steps at 14.
using e0 = rest::get<"/resources0/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e1 = rest::get<"/resources1/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e2 = rest::get<"/resources2/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e3 = rest::get<"/resources3/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e4 = rest::get<"/resources4/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e5 = rest::get<"/resources5/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e6 = rest::get<"/resources6/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e7 = rest::get<"/resources7/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e8 = rest::get<"/resources8/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e9 = rest::get<"/resources9/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e10 = rest::get<"/resources10/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e11 = rest::get<"/resources11/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e12 = rest::get<"/resources12/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e13 = rest::get<"/resources13/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e14 = rest::get<"/resources14/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e15 = rest::get<"/resources15/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e16 = rest::get<"/resources16/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e17 = rest::get<"/resources17/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e18 = rest::get<"/resources18/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e19 = rest::get<"/resources19/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e20 = rest::get<"/resources20/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e21 = rest::get<"/resources21/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e22 = rest::get<"/resources22/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e23 = rest::get<"/resources23/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e24 = rest::get<"/resources24/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e25 = rest::get<"/resources25/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e26 = rest::get<"/resources26/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e27 = rest::get<"/resources27/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e28 = rest::get<"/resources28/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e29 = rest::get<"/resources29/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e30 = rest::get<"/resources30/{id}/children/{child}/details", void, std::int64_t, std::string>;
using e31 = rest::get<"/resources31/{id}/children/{child}/details", void, std::int64_t, std::string>;

using large = rest::api<
    "/api/v1", rest::group<
                   e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21,
                   e22, e23, e24, e25, e26, e27, e28, e29, e30, e31>>;

static_assert(large::routes::size == 32);

} // namespace contract_test_size

namespace contract_test_types {

using get = quoll::rest::get<"/u/{uid}/e/{id}", void, std::int64_t, unsigned>;
using api = quoll::rest::api<"/api", get>;

// The types of a route's path parameters, as its own types say them.
static_assert(std::same_as<api::route<get>::path_types, quoll::rest::detail::type_list<std::int64_t, unsigned>>);
static_assert(api::route<get>::method == quoll::http::verb::get);

} // namespace contract_test_types
