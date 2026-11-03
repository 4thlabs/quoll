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
namespace mount_test {

namespace rest = quoll::rest;

using quoll::async::task;
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
  std::vector<std::string> tag;
};

struct not_found {
  std::string detail;
};

struct card {
  std::int64_t owner = 0;
  std::string request_id;
  std::optional<std::string> status;
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

// A list header: its values may come on several lines, or combined on one.
using tagged = endpoint<"cards.tagged", verb::get, "/tagged", header<"X-Tag", std::vector<std::string>>,
                        returns<std::vector<std::string>>>;

using api = rest::api<"/users/{user_id:int64}/cards", list, tagged>;
} // namespace cards

namespace keys {
using namespace quoll::rest;

using ping = endpoint<"keys.ping", verb::get, "/ping", security<api_key<"X-Api-Key">>,
                      returns<std::string, status::ok, text_codec>>;
using ping_query = endpoint<"keys.ping_query", verb::get, "/ping-query", security<api_key_query<"key">>,
                            returns<std::string, status::ok, text_codec>>;

using api = rest::api<"/keys", ping, ping_query>;
} // namespace keys

using v1 =
    rest::api<"/api/v1", auth::api, keys::api, rest::with<rest::security<rest::bearer_auth>, users::api, cards::api>>;

// Synchronous: returns the value itself.
struct auth_service {
  token operator()(auth::login, credentials const &given) const {
    if (given.password != "secret") {
      throw rest::http_error{status::unauthorized, "wrong password"};
    }

    return {"t-" + given.email};
  }
};

struct users_service {
  // Describes the filter it got in the name of the one user it returns.
  task<std::vector<user>> operator()(users::list, user_filter filter) const {
    std::string described = filter.limit ? std::to_string(*filter.limit) : "-";

    described += "|" + filter.name.value_or("-");

    for (auto const &tag : filter.tag) {
      described += "|" + tag;
    }

    co_return std::vector<user>{{.id = 1, .name = described, .email = std::nullopt}};
  }

  task<rest::reply<users::get>> operator()(users::get, std::int64_t id) const {
    if (id == 13) {
      throw std::runtime_error{"internal details that must not leak"};
    }

    if (id == 42) {
      co_return user{.id = 42, .name = "Ada", .email = "ada@example.com"};
    }

    co_return not_found{"no user " + std::to_string(id)};
  }

  task<user> operator()(users::create, new_user created) const {
    if (created.name == "taken") {
      throw rest::http_error{status::conflict, "name taken"};
    }

    if (created.name == "unauthorized") {
      throw rest::http_error{status::unauthorized, "expired session"};
    }

    if (created.name == "teapot") {
      throw rest::http_error{static_cast<status>(418), "short and stout"};
    }

    if (created.name == "fine") {
      throw rest::http_error{status::ok, "not an error"};
    }

    co_return user{.id = 7, .name = std::move(created.name), .email = std::move(created.email)};
  }

  task<> operator()(users::remove, std::int64_t) const { co_return; }
};

struct cards_service {
  std::vector<card> operator()(cards::list, std::int64_t owner, std::optional<std::string> state,
                               std::string request_id) const {
    return {{.owner = owner, .request_id = std::move(request_id), .status = std::move(state)}};
  }

  std::vector<std::string> operator()(cards::tagged, std::int64_t, std::vector<std::string> tags) const { return tags; }
};

struct keys_service {
  std::string operator()(keys::ping) const { return "pong"; }

  std::string operator()(keys::ping_query) const { return "pong from the query"; }
};

} // namespace mount_test

namespace {

using namespace mount_test;

namespace http = quoll::http;

constexpr rest::lenient_json_codec json;

http::router make_router() {
  http::router router;

  rest::mount<v1>(
      router, auth_service{}, users_service{}, cards_service{}, keys_service{},
      rest::validators([](rest::credential<rest::bearer_auth> const &given) { return given.value == "valid"; },
                       [](rest::credential<rest::api_key<"X-Api-Key">> const &given) -> task<bool> {
                         co_return given.value == "key";
                       },
                       [](rest::credential<rest::api_key_query<"key">> const &given) { return given.value == "key"; }));
  return router;
}

using field_list = std::vector<std::pair<std::string, std::string>>;

field_list const bearer{{"Authorization", "Bearer valid"}};

http::response send(http::router const &router, verb method, std::string target, field_list const &fields = bearer,
                    std::string body = {}) {
  http::request message;

  message.method = method;
  message.target = std::move(target);
  message.body = std::move(body);

  for (auto const &[name, value] : fields) {
    message.fields.add(name, value);
  }

  return quoll::test::run(router.dispatch(std::move(message))).value();
}

rest::problem problem_of(http::response const &response) {
  REQUIRE(response.fields.find("Content-Type") == "application/problem+json");

  auto decoded = json.decode<rest::problem>(response.body);

  REQUIRE(decoded.has_value());
  return *decoded;
}

} // namespace

TEST_CASE("mount: a request reaches its service, and the response its client", "[rest][mount]") {
  auto const router = make_router();
  auto const found = send(router, verb::get, "/api/v1/users/42");

  REQUIRE(found.status == http::status::ok);
  REQUIRE(found.fields.find("Content-Type") == "application/json");

  auto const ada = json.decode<user>(found.body);

  REQUIRE(ada.has_value());
  REQUIRE(ada->name == "Ada");
  REQUIRE(ada->email == "ada@example.com");
}

TEST_CASE("mount: a reply answers the status of the response it holds", "[rest][mount]") {
  auto const router = make_router();
  auto const missing = send(router, verb::get, "/api/v1/users/7");

  REQUIRE(missing.status == http::status::not_found);
  REQUIRE(json.decode<not_found>(missing.body)->detail == "no user 7");
}

TEST_CASE("mount: bearer credentials are required where with<> asks for them", "[rest][mount]") {
  auto const router = make_router();
  auto const anonymous = send(router, verb::get, "/api/v1/users/42", {});

  REQUIRE(anonymous.status == http::status::unauthorized);
  REQUIRE(anonymous.fields.find("WWW-Authenticate") == "Bearer");
  REQUIRE(problem_of(anonymous).detail == "missing credentials");

  auto const rejected = send(router, verb::get, "/api/v1/users/42", {{"Authorization", "Bearer forged"}});

  REQUIRE(rejected.status == http::status::unauthorized);
  REQUIRE(problem_of(rejected).detail == "rejected credentials");
  REQUIRE(send(router, verb::get, "/api/v1/users/42", {{"Authorization", "bearer valid"}}).status == http::status::ok);
  // Not under with<security<>>: no credentials needed.
  REQUIRE(send(router, verb::post, "/api/v1/auth/login", {{"Content-Type", "application/json"}},
               R"({"email":"ada","password":"secret"})")
              .status == http::status::ok);
}

TEST_CASE("mount: api keys, in a header or in the query", "[rest][mount]") {
  auto const router = make_router();
  auto const pong = send(router, verb::get, "/api/v1/keys/ping", {{"X-Api-Key", "key"}});

  REQUIRE(pong.status == http::status::ok);
  REQUIRE(pong.body == "pong");
  REQUIRE(pong.fields.find("Content-Type") == "text/plain; charset=utf-8");
  REQUIRE(send(router, verb::get, "/api/v1/keys/ping-query?key=key", {}).body == "pong from the query");

  // RFC 9110: a 401 always carries a challenge; API keys get one saying where the key goes.
  auto const rejected = send(router, verb::get, "/api/v1/keys/ping", {{"X-Api-Key", "other"}});

  REQUIRE(rejected.status == http::status::unauthorized);
  REQUIRE(rejected.fields.find("WWW-Authenticate") == R"(ApiKey header="X-Api-Key")");

  auto const missing = send(router, verb::get, "/api/v1/keys/ping-query", {});

  REQUIRE(missing.status == http::status::unauthorized);
  REQUIRE(missing.fields.find("WWW-Authenticate") == R"(ApiKey query="key")");
}

TEST_CASE("mount: path and query arguments are decoded, or answer 400", "[rest][mount]") {
  auto const router = make_router();
  auto const listed = send(router, verb::get, "/api/v1/users?limit=2&name=A+da&tag=a&tag=b%2Bc&other=x");

  REQUIRE(listed.status == http::status::ok);
  REQUIRE(json.decode<std::vector<user>>(listed.body)->front().name == "2|A da|a|b+c");
  REQUIRE(json.decode<std::vector<user>>(send(router, verb::get, "/api/v1/users").body)->front().name == "-|-");

  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users/abc")).detail == "invalid path parameter id");
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users?limit=x")).detail == "invalid query parameter limit");
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users?limit=1&limit=2")).detail ==
          "query parameter limit is repeated");
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users?name=%zz")).detail == "invalid query string");
}

TEST_CASE("mount: headers and inherited path parameters", "[rest][mount]") {
  auto const router = make_router();
  auto const listed = send(router, verb::get, "/api/v1/users/5/cards?status=active",
                           {{"Authorization", "Bearer valid"}, {"x-request-id", "r-1"}});

  REQUIRE(listed.status == http::status::ok);

  auto const cards = json.decode<std::vector<card>>(listed.body);

  REQUIRE(cards->front().owner == 5);
  REQUIRE(cards->front().request_id == "r-1");
  REQUIRE(cards->front().status == "active");
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users/5/cards")).detail == "missing header X-Request-Id");
}

TEST_CASE("mount: a body is decoded with its codec, of its media type", "[rest][mount]") {
  auto const router = make_router();
  auto post = [&](std::string content_type, std::string body) {
    auto fields = bearer;

    if (!content_type.empty()) {
      fields.emplace_back("Content-Type", std::move(content_type));
    }

    return send(router, verb::post, "/api/v1/users", fields, std::move(body));
  };

  auto const created = post("application/json; charset=utf-8", R"({"name":"Ada","email":"ada@example.com"})");

  REQUIRE(created.status == http::status::created);
  REQUIRE(json.decode<user>(created.body)->id == 7);

  auto const wrong_type = post("text/plain", "Ada");

  REQUIRE(wrong_type.status == http::status::unsupported_media_type);
  REQUIRE(problem_of(wrong_type).detail == "the body is application/json");
  REQUIRE(problem_of(post("", "")).detail == "missing body");
  REQUIRE(problem_of(post("application/json", "{")).detail->starts_with("invalid body: "));
  REQUIRE(post("application/json", R"({"name":"Ada"})").status == http::status::bad_request);
}

TEST_CASE("mount: errors thrown by services", "[rest][mount]") {
  auto const router = make_router();
  auto const conflict = send(router, verb::post, "/api/v1/users",
                             {{"Authorization", "Bearer valid"}, {"Content-Type", "application/json"}},
                             R"({"name":"taken","email":"x"})");

  REQUIRE(conflict.status == http::status::conflict);
  REQUIRE(problem_of(conflict).detail == "name taken");
  REQUIRE(problem_of(conflict).title == "Conflict");

  auto const failed = send(router, verb::get, "/api/v1/users/13");

  REQUIRE(failed.status == http::status::internal_server_error);
  REQUIRE_FALSE(problem_of(failed).detail.has_value());
  REQUIRE(problem_of(failed).status == 500);
}

TEST_CASE("mount: a service returning nothing answers 204", "[rest][mount]") {
  auto const router = make_router();
  auto const removed = send(router, verb::delete_, "/api/v1/users/42");

  REQUIRE(removed.status == http::status::no_content);
  REQUIRE(removed.body.empty());
}

TEST_CASE("mount: credentials come before arguments, and empty ones are missing", "[rest][mount]") {
  auto const router = make_router();

  REQUIRE(send(router, verb::get, "/api/v1/users?name=%zz", {}).status == http::status::unauthorized);
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/keys/ping", {{"X-Api-Key", ""}})).detail ==
          "missing credentials");
  REQUIRE(problem_of(send(router, verb::get, "/api/v1/keys/ping-query?key=", {})).detail == "missing credentials");
}

TEST_CASE("mount: a NUL in the query string is refused", "[rest][mount]") {
  auto const router = make_router();

  REQUIRE(problem_of(send(router, verb::get, "/api/v1/users?name=a%00b")).detail == "invalid query string");
  // The key cannot be read: missing, not "key".
  REQUIRE(send(router, verb::get, "/api/v1/keys/ping-query?key=key%00garbage", {}).status ==
          http::status::unauthorized);
}

TEST_CASE("mount: a list header takes its values from every line, split at commas", "[rest][mount]") {
  auto const router = make_router();
  auto const tagged = send(router, verb::get, "/api/v1/users/5/cards/tagged",
                           {{"Authorization", "Bearer valid"}, {"X-Tag", "a, b"}, {"X-Tag", "c"}, {"X-Tag", " , "}});

  REQUIRE(tagged.status == http::status::ok);
  REQUIRE(json.decode<std::vector<std::string>>(tagged.body) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("mount: a body without media type is 415, not missing", "[rest][mount]") {
  auto const router = make_router();
  auto const untyped = send(router, verb::post, "/api/v1/users", bearer, R"({"name":"Ada","email":"a"})");

  REQUIRE(untyped.status == http::status::unsupported_media_type);
}

TEST_CASE("mount: what http_error answers", "[rest][mount]") {
  auto const router = make_router();
  auto create = [&](std::string name) {
    return send(router, verb::post, "/api/v1/users",
                {{"Authorization", "Bearer valid"}, {"Content-Type", "application/json"}},
                R"({"name":")" + name + R"(","email":"x"})");
  };

  // On a secured route, a 401 gets the challenge of its scheme.
  auto const expired = create("unauthorized");

  REQUIRE(expired.status == http::status::unauthorized);
  REQUIRE(expired.fields.find("WWW-Authenticate") == "Bearer");
  // A status without a reason phrase: no title.
  auto const teapot = create("teapot");

  REQUIRE(static_cast<int>(teapot.status) == 418);
  REQUIRE_FALSE(problem_of(teapot).title.has_value());
  // Not an error status: a programming error, answered as one.
  REQUIRE(create("fine").status == http::status::internal_server_error);
}

TEST_CASE("mount: adds all the routes or none", "[rest][mount]") {
  http::router router;

  router.add(verb::get, "/api/v1/users/{x}", "taken",
             [](http::request, http::route_match) -> task<http::response> { co_return http::response{}; });

  REQUIRE_THROWS_AS(
      rest::mount<v1>(router, auth_service{}, users_service{}, cards_service{}, keys_service{},
                      rest::validators([](rest::credential<rest::bearer_auth> const &) { return true; },
                                       [](rest::credential<rest::api_key<"X-Api-Key">> const &) { return true; },
                                       [](rest::credential<rest::api_key_query<"key">> const &) { return true; })),
      std::invalid_argument);
  // Routes mount() would have added before the clash are not there.
  REQUIRE(send(router, verb::post, "/api/v1/auth/login").status == http::status::not_found);
}
