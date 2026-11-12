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
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/history.hpp>
#include <quoll/ui/route.hpp>
#include <quoll/ui/router.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Named, not unnamed: a route reads the names of its page's props, which Glaze, under Clang, cannot reflect in a type
// without linkage.
namespace router_test {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

template <class C>
using test_app = app<C, memory_renderer>;

std::vector<std::string> loaded;

task<std::string> load_product(std::int64_t id) {
  co_await quoll::async::sleep_for(1ms);
  loaded.push_back(std::to_string(id));
  co_return "product " + std::to_string(id);
}

struct product_page;

struct home_page {
  element view();
};

// Its products, under the outlet of its layout: the query's `q` filters them.
struct product_list {
  struct props {
    std::optional<std::string> q;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) { return p("list " + given.q.value_or("all")); }
};

// A product, from its route's `{id}`, which loads as it changes.
struct product_page {
  struct props {
    std::int64_t id = 0;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  state<std::string> text;
  effect load{&props::id, load_product, text};

  element view(props const &given) { return p("page " + std::to_string(given.id) + ": " + *text); }
};

struct products_layout {
  element view() { return section({h1("products"), outlet()}); }
};

// A ratio, from its route: a NaN or an infinity is no number an address names.
struct ratio_page {
  struct props {
    double x = 0;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) { return p("ratio " + std::to_string(given.x)); }
};

// A file, the rest of its route's path, which may start with '/'.
struct file_page {
  struct props {
    std::string path;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) { return p("file [" + given.path + "]"); }
};

struct missing_page {
  element view() { return p("missing"); }
};

inline constexpr auto shop_routes =
    quoll::ui::routes(route<"/", home_page>(),
                      route<"/products", products_layout>(route<"", product_list>(), route<"{id}", product_page>()),
                      route<"/ratio/{x}", ratio_page>(), route<"/files/{path:path}", file_page>(),
                      redirect<"/items/{id}", "/products/{id}">(), not_found<missing_page>());

element home_page::view() {
  return div({
      p("home"),
      link_to<product_page>({.id = 7}, "seven"),
      link_to("/products?q=tea", "tea"),
  });
}

// The app's own component: a link home, and the page of the location.
struct shell {
  element view() { return main_({link_to<home_page>("home"), outlet()}); }
};

[[nodiscard]] app_options at(std::string start) { return {.contexts = {routing<shop_routes>(std::move(start))}}; }

} // namespace router_test

using namespace router_test;

TEST_CASE("router: shows the page of the location, in the outlets of its layouts", "[ui][router]") {
  struct seen {
    std::string start;
    std::string shown;
  };

  std::vector<seen> pages = {
      {"/", "<p>home</p>"},
      {"/products", "<h1>products</h1><p>list all</p>"},
      {"/products?q=tea%20x", "<p>list tea x</p>"},
      {"/products/7", "<h1>products</h1><p>page 7: </p>"},
      {"/products/7/", "<p>page 7: </p>"},
      {"/products/seven", "<p>missing</p>"},
      {"/nowhere", "<p>missing</p>"},
      {"/ratio/0.5", "<p>ratio 0.500000</p>"},
      {"/ratio/nan", "<p>missing</p>"},
      {"/ratio/inf", "<p>missing</p>"},
  };

  for (auto const &[start, shown_there] : pages) {
    loaded.clear();

    virtual_time time;
    memory_renderer const shown;
    test_app<shell> instance{time.get_executor(), shown, at(start)};

    auto const act = []() -> task<> { co_await settle(); };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
    INFO(start << " shows " << shown.html());
    CHECK(shown.html().contains(shown_there));
  }
}

TEST_CASE("router: a link goes to its page, which the history goes back from", "[ui][router]") {
  loaded.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, at("/")};
  std::string opened;
  std::string back;

  auto const act = [&]() -> task<> {
    co_await settle();

    // A link to a page for props: its address, and a click on it.
    instance.dispatch(shown.find_text("seven"), "click");
    co_await quoll::async::sleep_for(5ms);
    opened = shown.html();

    instance.context<router>().back();
    co_await settle();
    back = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(opened.contains("<p>page 7: product 7</p>"));
  CHECK(back.contains("<p>home</p>"));
  CHECK(shown.html().contains("href=\"/products/7\""));
  CHECK(shown.html().contains("data-quoll-link"));
  CHECK(loaded == std::vector<std::string>{"7"});
}

TEST_CASE("router: a page for another value of its route's parameter loads again, the same page kept", "[ui][router]") {
  loaded.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, at("/products/7")};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.context<router>().go<product_page>({.id = 8});
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>page 8: product 8</p>"));
  CHECK(loaded == std::vector<std::string>{"7", "8"});
}

TEST_CASE("router: a redirect replaces its address in the history", "[ui][router]") {
  loaded.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, at("/")};
  std::string redirected;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<router>().go("/items/3?from=old");
    co_await settle();
    redirected = instance.context<router>().where->path + "?" + instance.context<router>().where->query;

    // Back from the page it redirected to: home, the old address gone.
    instance.context<router>().back();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(redirected == "/products/3?from=old");
  CHECK(shown.html().contains("<p>home</p>"));
}

TEST_CASE("router: the address of a page, from its props, its other fields in the query", "[ui][router]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, at("/")};
  auto const &nav = instance.context<router>();

  CHECK(nav.href<product_page>({.id = 42}) == "/products/42");
  CHECK(nav.href<product_list>({.q = std::nullopt}) == "/products");
  CHECK(nav.href<product_list>({.q = "tea & more"}) == "/products?q=tea%20%26%20more");
  CHECK(nav.href<home_page>() == "/");
  CHECK_THROWS_AS(nav.href<shell>(), std::logic_error);
}

TEST_CASE("router: an address written without its first '/' is the same, pushed and matched", "[ui][router]") {
  // A history of the test's, which outlives the app, to read what it was given.
  memory_history history;
  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, {.contexts = {routing<shop_routes>(history.ref())}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<router>().go("products/5/");
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(history.current() == "/products/5");
  CHECK(history.ref().target<memory_history>() == &history);
  CHECK(instance.context<router>().where->path == "/products/5");
  CHECK(shown.html().contains("<p>page 5: product 5</p>"));
}

TEST_CASE("router: the rest of a path keeps its segments as they were, an empty first one included", "[ui][router]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, at("/")};
  std::string href;

  auto const act = [&]() -> task<> {
    co_await settle();
    href = instance.context<router>().href<file_page>({.path = "/docs/report.txt"});
    instance.context<router>().go(href);
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(href == "/files//docs/report.txt");
  CHECK(shown.html().contains("<p>file [/docs/report.txt]</p>"));
}

TEST_CASE("router: an address has one form, its dot segments resolved, pushed and matched alike", "[ui][router]") {
  memory_history history;
  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, {.contexts = {routing<shop_routes>(history.ref())}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<router>().go("./products/./9/../5/");
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(history.current() == "/products/5");
  CHECK(shown.html().contains("<p>page 5: product 5</p>"));
}

TEST_CASE("router: going to the address shown, or to one that redirects there, pushes nothing", "[ui][router]") {
  memory_history history{"/products/5"};
  virtual_time time;
  memory_renderer const shown;
  test_app<shell> instance{time.get_executor(), shown, {.contexts = {routing<shop_routes>(history.ref())}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<router>().go("/products/5");
    instance.context<router>().go<product_page>({.id = 5});

    // One that redirects to it, as /items/5.
    instance.context<router>().go("/items/5");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(history.size() == 1);
}
