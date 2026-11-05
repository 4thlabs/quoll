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

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

TEST_CASE("router: a dot segment behind an encoded slash reaches no handler", "[http][router]") {
  auto const router = make_router();

  REQUIRE(dispatch(router, verb::get, "/users/..%2F..%2Fetc%2Fpasswd").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/users/a%2F..%2Fb").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/users/..%5Cwindows").status == http::status::bad_request);
  REQUIRE(dispatch(router, verb::get, "/users/.%2Fa").status == http::status::bad_request);
  // Dots that are not a whole component stay.
  REQUIRE(dispatch(router, verb::get, "/users/a..b%2Fc.d").body == "users.get\na..b/c.d\n?");
}

namespace {

// Adds its name to a field on the way in, and to the body on the way out.
http::router::middleware tagging(std::string name) {
  return [name = std::move(name)](http::request message, http::next next) -> task<http::response> {
    message.fields.add("X-Seen", name);

    auto answer = co_await next(std::move(message));

    answer.body += " " + name;
    co_return answer;
  };
}

} // namespace

TEST_CASE("router: middlewares run around a route, the first added outermost", "[http][router]") {
  http::router router;
  std::string seen;

  router.add(verb::get, "/a", "a", [&seen](http::request message, http::route_match) -> task<http::response> {
    for (auto const &field : message.fields) {
      seen += field.value;
    }

    http::response answer;

    answer.body = "route";
    co_return answer;
  });
  router.use(tagging("outer"));
  router.use(tagging("inner"));

  REQUIRE(dispatch(router, verb::get, "/a").body == "route inner outer");
  REQUIRE(seen == "outerinner");
}

TEST_CASE("router: a middleware sees what the router answers by itself", "[http][router]") {
  http::router router;
  std::vector<http::status> statuses;

  router.add(verb::get, "/a", "a", echo);
  router.use([&statuses](http::request message, http::next next) -> task<http::response> {
    auto answer = co_await next(std::move(message));

    statuses.push_back(answer.status);
    co_return answer;
  });

  REQUIRE(dispatch(router, verb::get, "/nothing").status == http::status::not_found);
  REQUIRE(dispatch(router, verb::post, "/a").status == http::status::method_not_allowed);
  REQUIRE(dispatch(router, verb::get, "bad").status == http::status::bad_request);
  REQUIRE(statuses ==
          std::vector{http::status::not_found, http::status::method_not_allowed, http::status::bad_request});
}

TEST_CASE("router: a middleware may answer in place of what comes next", "[http][router]") {
  http::router router;
  int reached = 0;

  router.add(verb::get, "/a", "a", [&reached](http::request, http::route_match) -> task<http::response> {
    ++reached;
    co_return http::response{};
  });
  router.use(tagging("outer"));
  router.use([](http::request message, http::next next) -> task<http::response> {
    if (!message.fields.contains("X-Key")) {
      http::response refused;

      refused.status = http::status::unauthorized;
      refused.body = "refused";
      co_return refused;
    }

    co_return co_await next(std::move(message));
  });
  router.use(tagging("never"));

  auto const answer = dispatch(router, verb::get, "/a");

  REQUIRE(answer.status == http::status::unauthorized);
  // Those outside it still run.
  REQUIRE(answer.body == "refused outer");
  REQUIRE(reached == 0);
}

TEST_CASE("router: what a handler throws goes through the middlewares, which may catch it", "[http][router]") {
  http::router router;

  router.add(verb::get, "/a", "a", [](http::request, http::route_match) -> task<http::response> {
    throw std::runtime_error{"failed"};
    co_return http::response{};
  });

  SECTION("not caught") {
    router.use(tagging("outer"));

    REQUIRE_THROWS_AS(dispatch(router, verb::get, "/a"), std::runtime_error);
  }

  SECTION("caught") {
    router.use([](http::request message, http::next next) -> task<http::response> {
      try {
        co_return co_await next(std::move(message));
      } catch (std::runtime_error const &failure) {
        http::response answer;

        answer.status = http::status::internal_server_error;
        answer.body = failure.what();
        co_return answer;
      }
    });

    REQUIRE(dispatch(router, verb::get, "/a").body == "failed");
  }

  SECTION("thrown by a handler before its task: out of next's task, not its call") {
    http::router other;

    other.add(verb::get, "/a", "a",
              [](http::request, http::route_match) -> task<http::response> { throw std::runtime_error{"before"}; });
    other.use([](http::request message, http::next next) -> task<http::response> {
      auto pending = next(std::move(message));

      try {
        co_return co_await std::move(pending);
      } catch (std::runtime_error const &failure) {
        http::response answer;

        answer.body = failure.what();
        co_return answer;
      }
    });

    REQUIRE(dispatch(other, verb::get, "/a").body == "before");
  }

  SECTION("thrown by a middleware before its task") {
    router.use([](http::request, http::next) -> task<http::response> { throw std::runtime_error{"before"}; });

    REQUIRE_THROWS_AS(dispatch(router, verb::get, "/a"), std::runtime_error);
  }
}

TEST_CASE("router: what a middleware changes reaches the handler, on the route already chosen", "[http][router]") {
  http::router router;
  std::string target_seen;

  router.add(verb::get, "/old", "old",
             [&target_seen](http::request message, http::route_match) -> task<http::response> {
               target_seen = message.target;
               co_return http::response{};
             });
  router.add(verb::get, "/new", "new", echo);
  router.use([](http::request message, http::next next) -> task<http::response> {
    message.target = "/new";
    co_return co_await next(std::move(message));
  });

  REQUIRE(dispatch(router, verb::get, "/old").status == http::status::ok);
  REQUIRE(target_seen == "/new");
}

TEST_CASE("router: next says what routing found", "[http][router]") {
  http::router router;
  std::string path;
  std::string route;
  std::vector<std::string> params;

  router.add(verb::get, "/users/{id}/cards/{card}", "cards.get", echo);
  router.add(verb::get, "/", "root", echo);
  router.use([&](http::request message, http::next next) -> task<http::response> {
    path = next.path();
    route = next.route();
    params.assign(next.params().begin(), next.params().end());
    co_return co_await next(std::move(message));
  });

  SECTION("a route") {
    REQUIRE(dispatch(router, verb::get, "/users/4%32/cards/a%20b?x=1").status == http::status::ok);
    REQUIRE(path == "/users/42/cards/a b");
    REQUIRE(route == "cards.get");
    REQUIRE(params == std::vector<std::string>{"42", "a b"});
  }

  SECTION("the root, and a target in absolute form") {
    REQUIRE(dispatch(router, verb::get, "http://example.org/").status == http::status::ok);
    REQUIRE(path == "/");
    REQUIRE(route == "root");
  }

  SECTION("no route: a path, and no name") {
    REQUIRE(dispatch(router, verb::get, "/nothing/here/").status == http::status::not_found);
    REQUIRE(path == "/nothing/here/");
    REQUIRE(route.empty());
    REQUIRE(params.empty());
  }

  SECTION("another method: no name, and the router's 405") {
    auto const refused = dispatch(router, verb::post, "/");

    REQUIRE(refused.status == http::status::method_not_allowed);
    REQUIRE(refused.fields.find("Allow") == "GET, HEAD");
    REQUIRE(path == "/");
    REQUIRE(route.empty());
  }

  SECTION("a target the router refuses: nothing") {
    for (std::string const target : {"bad", "/a/%2e%2e/b", "/a%00", "/%zz"}) {
      CAPTURE(target);
      REQUIRE(dispatch(router, verb::get, target).status == http::status::bad_request);
      REQUIRE(path.empty());
      REQUIRE(route.empty());
    }
  }
}

TEST_CASE("router: a check on next.path cannot be bypassed by how a target is written", "[http][router]") {
  http::router router;
  int reached = 0;

  router.add(verb::get, "/admin/users", "admin", [&reached](http::request, http::route_match) -> task<http::response> {
    ++reached;
    co_return http::response{};
  });
  router.use([](http::request message, http::next next) -> task<http::response> {
    if (next.path().starts_with("/admin")) {
      http::response refused;

      refused.status = http::status::forbidden;
      co_return refused;
    }

    co_return co_await next(std::move(message));
  });

  for (std::string const target : {"/admin/users", "/%61dmin/users", "/admin/%75sers", "http://x/admin/users"}) {
    CAPTURE(target);
    REQUIRE(dispatch(router, verb::get, target).status == http::status::forbidden);
  }

  REQUIRE(reached == 0);
}

TEST_CASE("router: an encoded slash cannot pass for a path that it is not", "[http][router]") {
  http::router router;
  std::string path;

  router.add(verb::get, "/{tenant}/reports", "reports", echo);
  router.use([&path](http::request message, http::next next) -> task<http::response> {
    path = next.path();
    co_return co_await next(std::move(message));
  });

  // Would read as under `/public/`, and is a report of the tenant `public/x`.
  REQUIRE(dispatch(router, verb::get, "/public%2Fx/reports").body == "reports\npublic/x\n?");
  REQUIRE_FALSE(path.starts_with("/public/"));
  REQUIRE(path == "/public%2Fx/reports");

  // And another tenant has another path.
  REQUIRE(dispatch(router, verb::get, "/public%252Fx/reports").body == "reports\npublic%2Fx\n?");
  REQUIRE(path == "/public%252Fx/reports");
}

TEST_CASE("router: an empty name would read as no route, and is refused", "[http][router]") {
  http::router router;

  REQUIRE_THROWS_AS(router.add(verb::get, "/a", "", echo), std::invalid_argument);
}

TEST_CASE("router: a middleware that only forwards need not be a coroutine", "[http][router]") {
  http::router router;

  router.add(verb::get, "/a", "a", echo);
  router.use([](http::request message, http::next const &next) { return next(std::move(message)); });

  REQUIRE(dispatch(router, verb::get, "/a").body == "a\n?");
}

TEST_CASE("router: middlewares and handlers that wait keep what they were given", "[http][router][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::router router;

  router.add(verb::get, "/users/{id}", "user", [](http::request, http::route_match match) -> task<http::response> {
    co_await quoll::async::sleep_for(1s);

    http::response answer;

    answer.body = match.params.at(0);
    co_return answer;
  });
  router.use([](http::request message, http::next next) -> task<http::response> {
    std::string const route{next.route()};

    co_await quoll::async::sleep_for(1s);

    auto answer = co_await next(std::move(message));

    co_await quoll::async::sleep_for(1s);
    answer.body += " " + route + " " + std::string{next.path()};
    co_return answer;
  });

  http::request message;

  message.target = "/users/7";

  auto const answer = quoll::test::run(time, router.dispatch(std::move(message))).value();

  REQUIRE(answer.body == "7 user /users/7");
  REQUIRE(time.now() == 3s);
}

namespace {

// A view it may keep, and return: it is valid until the middleware's task ends, whatever becomes of this copy of next.
std::string_view path_of(http::next const next) { return next.path(); }

} // namespace

static_assert(!std::constructible_from<http::local_transport, http::router &&>);
static_assert(std::constructible_from<http::local_transport, http::router &>);

TEST_CASE("router: what a coroutine takes by reference lives until its task ends", "[http][router][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::router router;
  std::string const long_text(100, 'x');
  auto const wait = []() -> task<> { co_await quoll::async::sleep_for(1s); };

  router.add(verb::get, "/{name}", "name",
             [&wait](http::request const &message, http::route_match const &match) -> task<http::response> {
               co_await wait();

               http::response answer;

               answer.body = message.target + " " + match.params.at(0);
               co_return answer;
             });

  auto expected = "/" + long_text + " " + long_text;
  auto waited = 1s;

  SECTION("without a middleware") {}

  SECTION("in a middleware") {
    expected += " /" + long_text;
    waited = 2s;
    router.use([&wait](http::request const &message, http::next const &next) -> task<http::response> {
      auto const path = path_of(next);

      co_await wait();

      auto answer = co_await next(http::request{message});

      answer.body += " " + std::string{path};
      co_return answer;
    });
  }

  SECTION("in a generic one") {
    expected += " /" + long_text;
    waited = 2s;
    router.use([&wait](auto &&message, auto const &next) -> task<http::response> {
      auto const path = path_of(next);

      co_await wait();

      auto answer = co_await next(std::move(message));

      answer.body += " " + std::string{path};
      co_return answer;
    });
  }

  http::request message;

  message.target = "/" + long_text;

  auto const answer = quoll::test::run(time, router.dispatch(std::move(message))).value();

  REQUIRE(answer.body == expected);
  REQUIRE(time.now() == waited);
}

TEST_CASE("local_transport: what a middleware throws comes out, and so does a stop", "[http][router]") {
  using namespace std::chrono_literals;

  http::router router;

  router.get("/thrown", [] { return http::response{}; });
  router.get("/waits", []() -> task<http::response> {
    co_await quoll::async::sleep_for(1h);
    co_return http::response{};
  });
  router.use([](http::request message, http::next next) -> task<http::response> {
    if (next.route() == "/thrown") {
      throw std::runtime_error{"failed"};
    }

    co_return co_await next(std::move(message));
  });

  http::local_transport const transport{router};

  auto const request_of = [](std::string target) {
    http::request message;

    message.target = std::move(target);
    return message;
  };

  REQUIRE_THROWS_AS(quoll::test::run(transport.send(request_of("/thrown"))), std::runtime_error);

  quoll::test::virtual_time time;

  // The time out reaches the caller, rather than a response.
  REQUIRE_THROWS_AS(quoll::test::run(time, quoll::async::with_timeout(1s, transport.send(request_of("/waits")))),
                    std::system_error);
}

TEST_CASE("router: a route is served again when a middleware calls next twice", "[http][router]") {
  http::router router;

  router.add(verb::get, "/users/{id}", "user", echo);
  router.use([](http::request message, http::next next) -> task<http::response> {
    static_cast<void>(co_await next(message));
    co_return co_await next(std::move(message));
  });

  REQUIRE(dispatch(router, verb::get, "/users/7").body == "user\n7\n?");
}

TEST_CASE("router: an empty middleware is refused", "[http][router]") {
  http::router router;

  REQUIRE_THROWS_AS(router.use({}), std::invalid_argument);
}

TEST_CASE("router: a copy has the middlewares, and its next points to the copy", "[http][router]") {
  http::router router;

  router.add(verb::get, "/a", "a", echo);
  router.use(tagging("one"));

  auto const copy = router;

  router.use(tagging("two"));

  REQUIRE(dispatch(copy, verb::get, "/a").body == "a\n? one");
  REQUIRE(dispatch(router, verb::get, "/a").body == "a\n? two one");
}

namespace {

task<http::response> free_function(std::int64_t id) {
  http::response answer;

  answer.body = std::to_string(id);
  co_return answer;
}

http::response with_text(std::string text) {
  http::response answer;

  answer.body = std::move(text);
  return answer;
}

} // namespace

TEST_CASE("router: a handler takes the parameters of its path, read as their types", "[http][router]") {
  http::router router;

  router.get("/users/{id}/cards/{card}", [](http::request message, std::int64_t id, std::string card) {
    return with_text(message.target + " " + std::to_string(id) + " " + card);
  });
  router.get("/numbers/{a}/{b}/{c}/{d}", [](int a, unsigned b, double c, bool d) {
    return with_text(std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + " " + (d ? "yes" : "no"));
  });
  router.get("/files/{name:path}", [](std::string name) { return with_text(name); });
  router.get("/typed/{id}", free_function);
  router.get("/none", [] { return with_text("none"); });

  REQUIRE(dispatch(router, verb::get, "/users/42/cards/a%20b").body == "/users/42/cards/a%20b 42 a b");
  REQUIRE(dispatch(router, verb::get, "/numbers/-1/2/0.5/true").body == "-1 2 0.500000 yes");
  REQUIRE(dispatch(router, verb::get, "/files/a/b/c").body == "a/b/c");
  REQUIRE(dispatch(router, verb::get, "/typed/7").body == "7");
  REQUIRE(dispatch(router, verb::get, "/none").body == "none");
}

TEST_CASE("router: a handler may be written in any of these ways", "[http][router]") {
  http::router router;

  router.get("/noexcept/{id}", [](int id) noexcept { return with_text(std::to_string(id)); });
  router.get("/pointer/{id}", +[](int id) noexcept { return with_text(std::to_string(id)); });
  router.get("/moved/{name}", [](std::string &&name) { return with_text(std::move(name)); });
  router.get("/edited/{name}", [](std::string &name) {
    name += "!";
    return with_text(name);
  });
  router.get("/request/{id}", [](http::request &message, int) {
    message.target += "!";
    return with_text(message.target);
  });

  REQUIRE(dispatch(router, verb::get, "/noexcept/1").body == "1");
  REQUIRE(dispatch(router, verb::get, "/pointer/3").body == "3");
  REQUIRE(dispatch(router, verb::get, "/moved/a").body == "a");
  REQUIRE(dispatch(router, verb::get, "/edited/a").body == "a!");
  REQUIRE(dispatch(router, verb::get, "/request/4").body == "/request/4!");
}

// Alone in its function: GCC 13 cannot name a static lambda beside another of the same signature.
TEST_CASE("router: a handler may be a static lambda", "[http][router]") {
  http::router router;

  router.get("/static/{id}", [](long id) static { return with_text(std::to_string(id)); });

  REQUIRE(dispatch(router, verb::get, "/static/2").body == "2");
}

TEST_CASE("router: a parameter that cannot be read as its type answers 400", "[http][router]") {
  http::router router;
  int reached = 0;

  router.get("/a/{id}/{flag}", [&reached](int, bool) {
    ++reached;
    return http::response{};
  });

  for (std::string const target : {"/a/x/true", "/a/1/maybe", "/a/1.5/true", "/a/99999999999/true", "/a/+1/true"}) {
    CAPTURE(target);
    REQUIRE(dispatch(router, verb::get, target).status == http::status::bad_request);
  }

  REQUIRE(reached == 0);
  REQUIRE(dispatch(router, verb::get, "/a/1/true").status == http::status::ok);
  REQUIRE(reached == 1);
}

TEST_CASE("router: references and views stay valid in a coroutine, until its task ends", "[http][router][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::router router;

  router.get("/a/{name}/{other}",
             [](http::request const &message, std::string const &name, std::string_view other) -> task<http::response> {
               co_await quoll::async::sleep_for(1s);
               co_return with_text(message.target + " " + name + " " + std::string{other});
             });

  http::request message;

  message.target = "/a/" + std::string(100, 'x') + "/" + std::string(100, 'y');

  auto const answer = quoll::test::run(time, router.dispatch(std::move(message))).value();

  REQUIRE(answer.body == "/a/" + std::string(100, 'x') + "/" + std::string(100, 'y') + " " + std::string(100, 'x') +
                             " " + std::string(100, 'y'));
}

TEST_CASE("router: a handler may keep a state of its own", "[http][router]") {
  http::router router;

  router.get("/count", [count = 0]() mutable { return with_text(std::to_string(++count)); });

  REQUIRE(dispatch(router, verb::get, "/count").body == "1");
  REQUIRE(dispatch(router, verb::get, "/count").body == "2");
}

TEST_CASE("router: what such a handler throws comes out of the task", "[http][router]") {
  http::router router;

  router.get("/a", []() -> http::response { throw std::runtime_error{"failed"}; });

  auto pending = [&router]() {
    http::request message;

    message.target = "/a";
    return router.dispatch(std::move(message));
  }();

  REQUIRE_THROWS_AS(quoll::test::run(std::move(pending)), std::runtime_error);
}

TEST_CASE("router: a route per method, named after its path", "[http][router]") {
  http::router router;

  auto const name = [](std::string id) { return with_text(id); };

  router.get("/things/{id}", name);
  router.post("/things/{id}", name);
  router.put("/things/{id}", name);
  router.patch("/things/{id}", name);
  router.delete_("/things/{id}", name);
  router.head("/things/{id}", name);
  router.options("/things/{id}", name);

  for (auto const method : {verb::get, verb::post, verb::put, verb::patch, verb::delete_, verb::head, verb::options}) {
    REQUIRE(dispatch(router, method, "/things/7").body == "7");
  }

  // Another method has no route here.
  REQUIRE(dispatch(router, verb::trace, "/things/7").status == http::status::method_not_allowed);

  // What add() refuses at run time, as what the compiler cannot see, these refuse too.
  REQUIRE_THROWS_AS(router.get("/things/{other}", name), std::invalid_argument);
}

TEST_CASE("router: the name of such a route is what a middleware counts by", "[http][router]") {
  http::router router;
  std::string route;

  router.get("/things/{id}", [](std::int64_t id) { return with_text(std::to_string(id)); });
  router.use([&route](http::request message, http::next next) -> task<http::response> {
    route = next.route();
    co_return co_await next(std::move(message));
  });

  REQUIRE(dispatch(router, verb::get, "/things/7").body == "7");
  REQUIRE(route == "/things/{id}");
}

TEST_CASE("router: a handler taking a body_reader reads the body of a request already whole", "[http][router]") {
  http::router router;

  router.post("/up/{name}",
              [](http::request request, std::string name, http::body_reader body) -> task<http::response> {
                http::response response;

                response.body = name + ":" + std::to_string(request.body.size()) + ":";

                while (auto const piece = co_await body.read()) {
                  response.body += *piece;
                }

                REQUIRE_FALSE(co_await body.read());
                co_return response;
              });
  router.post("/whole", [](http::request request) {
    http::response response;

    response.body = std::move(request.body);
    return response;
  });

  http::request message;

  message.method = http::verb::post;
  message.target = "/up/a";
  message.body = "a body long enough to be out of a small string";

  REQUIRE(router.reads_body_in_pieces(message));
  REQUIRE(quoll::test::run(router.dispatch(message))->body == "a:0:" + message.body);

  message.target = "/whole";
  REQUIRE_FALSE(router.reads_body_in_pieces(message));
  message.target = "/nowhere";
  REQUIRE_FALSE(router.reads_body_in_pieces(message));

  http::body_reader const nothing;

  REQUIRE_FALSE(nothing);

  auto const read_nothing = [&]() -> task<bool> { co_return (co_await nothing.read()).has_value(); };

  REQUIRE_FALSE(*quoll::test::run(read_nothing()));
}

TEST_CASE("router: a request that cannot be for such a route does not look", "[http][router]") {
  http::router router;

  router.get("/a", [](http::request) { return http::response{}; });

  http::request message;

  // Not even a target it could read.
  message.target = "*";
  REQUIRE_FALSE(router.reads_body_in_pieces(message));

  // Nor for another method than such a route's.
  router.post("/b", [](http::body_reader) { return http::response{}; });
  message.target = "/b";
  REQUIRE_FALSE(router.reads_body_in_pieces(message));
  message.method = http::verb::post;
  REQUIRE(router.reads_body_in_pieces(message));
}

TEST_CASE("router: what dispatch() is given to read the body reaches the handler, not the request", "[http][router]") {
  struct one_piece {
    bool read_once = false;

    [[nodiscard]] task<std::optional<std::string_view>> read() {
      if (std::exchange(read_once, true)) {
        co_return std::nullopt;
      }

      co_return std::string_view{"from the stream"};
    }
  };

  http::router router;

  router.post("/up", [](http::request request, http::body_reader body) -> task<http::response> {
    http::response response;

    response.body = request.body + "|";

    while (auto const piece = co_await body.read()) {
      response.body += *piece;
    }

    co_return response;
  });
  router.use([](http::request request, http::next const &next) { return next(std::move(request)); });

  http::request message;

  message.method = http::verb::post;
  message.target = "/up";

  one_piece source;

  auto const answered = quoll::test::run(router.dispatch(std::move(message), http::body_reader{source}));

  REQUIRE(answered->body == "|from the stream");
}

TEST_CASE("router: a body read through next twice is read once, with or without a stream", "[http][router]") {
  http::router router;

  router.post("/up", [](http::body_reader body) -> task<http::response> {
    http::response response;

    while (auto const piece = co_await body.read()) {
      response.body += *piece;
    }

    co_return response;
  });
  router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto first = co_await next(request);
    auto second = co_await next(std::move(request));

    first.body += "|" + second.body;
    co_return first;
  });

  http::request message;

  message.method = http::verb::post;
  message.target = "/up";
  message.body = "abc";

  auto const answered = quoll::test::run(router.dispatch(std::move(message)));

  REQUIRE(answered->body == "abc|");
}

TEST_CASE("router: a HEAD request reads its body as the GET route it goes to does", "[http][router]") {
  http::router router;

  router.get("/a", [](http::body_reader) { return http::response{}; });
  router.get("/b", [](http::request) { return http::response{}; });

  http::request message;

  message.method = http::verb::head;
  message.target = "/a";
  REQUIRE(router.reads_body_in_pieces(message));

  message.target = "/b";
  REQUIRE_FALSE(router.reads_body_in_pieces(message));
}

TEST_CASE("body_reader: target() recovers what it reads through", "[http][router]") {
  struct first_source {
    [[nodiscard]] task<std::optional<std::string_view>> read() { co_return std::nullopt; }
  };

  struct second_source {
    [[nodiscard]] task<std::optional<std::string_view>> read() { co_return std::nullopt; }
  };

  first_source source;
  http::body_reader const reader{source};
  http::body_reader const nothing;

  REQUIRE(reader.target<first_source>() == &source);
  REQUIRE(reader.target<second_source>() == nullptr);
  REQUIRE(nothing.target<first_source>() == nullptr);
}
