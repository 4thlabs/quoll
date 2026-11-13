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
#include <quoll/ui/routing/history.hpp>
#include <quoll/ui/routing/route.hpp>
#include <quoll/ui/routing/router.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace router_test {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

std::vector<std::string> loaded;

task<std::string> load_product(std::int64_t id) {
  co_await quoll::async::sleep_for(1ms);
  loaded.push_back(std::to_string(id));
  co_return "product " + std::to_string(id);
}

// Its products, under the outlet of its layout: the query's `q` filters them.
struct product_list_props {
  std::optional<std::string> q;

  [[nodiscard]] bool operator==(product_list_props const &) const = default;
};

constexpr component<product_list_props> product_list = [](product_list_props const &given) {
  return p("list " + given.q.value_or("all"));
};

// A product, from its route's `{id}`, which loads as it changes.
struct product_page_props {
  std::int64_t id = 0;

  [[nodiscard]] bool operator==(product_page_props const &) const = default;
};

// Declared, as a header would, and defined below, as a source of its own would.
extern component<product_page_props> const product_page;

constexpr component<> home_page = [] {
  return div({
      p("home"),
      link_to(product_page, {.id = 7}, "seven"),
      link_to("/products?q=tea", "tea"),
  });
};

constexpr component<> products_layout = [] { return section({h1("products"), outlet()}); };

// A ratio, from its route: a NaN or an infinity is no number an address names.
struct ratio_page_props {
  double x = 0;

  [[nodiscard]] bool operator==(ratio_page_props const &) const = default;
};

constexpr component<ratio_page_props> ratio_page = [](ratio_page_props const &given) {
  return p("ratio " + std::to_string(given.x));
};

// A file, the rest of its route's path, which may start with '/'.
struct file_page_props {
  std::string path;

  [[nodiscard]] bool operator==(file_page_props const &) const = default;
};

constexpr component<file_page_props> file_page = [](file_page_props const &given) {
  return p("file [" + given.path + "]");
};

constexpr component<> missing_page = [] { return p("missing"); };

inline constexpr auto shop_routes = quoll::ui::routes(
    route<"/", home_page>(),
    route<"/products", products_layout>(
        route<"", product_list, from_query<"q", &product_list_props::q>>(),
        route<"{id}", product_page, &product_page_props::id>()
    ),
    route<"/ratio/{x}", ratio_page, &ratio_page_props::x>(),
    route<"/files/{path:path}", file_page, &file_page_props::path>(), redirect<"/items/{id}", "/products/{id}">(),
    not_found<missing_page>()
);

constexpr component<product_page_props> product_page = [](product_page_props const &given) {
  auto &text = use_state<std::string>();

  use_effect(given.id, load_product, text);
  return p("page " + std::to_string(given.id) + ": " + *text);
};

// The app's root: a link home, and the page of the location.
constexpr component<> shell = [] { return main_({link_to(home_page, "home"), outlet()}); };

[[nodiscard]] app_options at(std::string start) { return {.contexts = {routing<shop_routes>(std::move(start))}}; }

// A layout reading, by its name, the parameter of the page it shows, and a field of the query.
struct shelf_props {
  std::optional<std::int64_t> id;
  std::string sort;

  [[nodiscard]] bool operator==(shelf_props const &) const = default;
};

constexpr component<shelf_props> shelf = [](shelf_props const &given) {
  auto const chosen = given.id ? std::to_string(*given.id) : std::string{"none"};

  return section({p("shelf " + chosen + " " + given.sort), outlet()});
};

// A page whose `note` no route names: the query does not fill it.
struct item_props {
  std::int64_t id = 0;
  std::string note;

  [[nodiscard]] bool operator==(item_props const &) const = default;
};

constexpr component<item_props> item_page = [](item_props const &given) {
  return p("item " + std::to_string(given.id) + " [" + given.note + "]");
};

constexpr component<> shelf_index = [] { return p("pick one"); };

// A cell, from two parameters of its path, a field of the query named before them.
struct cell_props {
  std::int64_t row = 0;
  std::int64_t col = 0;
  std::int64_t zoom = 1;

  [[nodiscard]] bool operator==(cell_props const &) const = default;
};

constexpr component<cell_props> cell_page = [](cell_props const &given) {
  return p("cell " + std::to_string(given.row) + " " + std::to_string(given.col) + " " + std::to_string(given.zoom));
};

// The root, with a field of the query.
struct start_props {
  std::string tab;

  [[nodiscard]] bool operator==(start_props const &) const = default;
};

constexpr component<start_props> start_page = [](start_props const &given) { return p("start " + given.tab); };

// A layout taking as a number the parameter its page takes as a string.
struct tagged_props {
  std::int64_t count = 0;

  [[nodiscard]] bool operator==(tagged_props const &) const = default;
};

constexpr component<tagged_props> tagged = [](tagged_props const &given) {
  return section({p("tagged " + std::to_string(given.count)), outlet()});
};

struct tag_props {
  std::string name;

  [[nodiscard]] bool operator==(tag_props const &) const = default;
};

constexpr component<tag_props> tag_page = [](tag_props const &given) { return p("tag " + given.name); };

// Shown at no route, where the address came from in its query.
struct lost_props {
  std::string from;

  [[nodiscard]] bool operator==(lost_props const &) const = default;
};

constexpr component<lost_props> lost_page = [](lost_props const &given) { return p("lost " + given.from); };

// clang-format off
inline constexpr auto shelf_routes = quoll::ui::routes(
    route<"/", start_page, from_query<"tab", &start_props::tab>>(),
    route<"/shelf", shelf, from_path<"id", &shelf_props::id>, from_query<"sort", &shelf_props::sort>>(
        route<"", shelf_index>(),
        route<"{id}", item_page, &item_props::id>()),
    route<"/grid/{row}/at/{col}", cell_page, from_query<"zoom", &cell_props::zoom>, &cell_props::row, &cell_props::col>(),
    route<"/tagged", tagged, from_path<"name", &tagged_props::count>>(
        route<"{name}", tag_page, &tag_props::name>()),
    not_found<lost_page, from_query<"from", &lost_props::from>>());
// clang-format on

constexpr component<> shelf_root = [] { return main_({outlet()}); };

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
    test_app instance{time.get_executor(), shown, shell, at(start)};

    auto const act = []() -> task<> { co_await settle(); };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
    INFO(start << " shows " << shown.html());
    CHECK(shown.html().contains(shown_there));
  }
}

TEST_CASE(
    "router: a route fills the fields it names, by the path's rank, or by name from the query and from the path "
    "matched",
    "[ui][router]"
) {
  struct seen {
    std::string start;
    std::string html;
  };

  std::vector<seen> const pages{
      {"/shelf/7?sort=price&note=x", "<main><section><p>shelf 7 price</p><p>item 7 []</p></section></main>"},
      {"/shelf", "<main><section><p>shelf none </p><p>pick one</p></section></main>"},
      // Each parameter in the field of its rank, the query's named before them.
      {"/grid/2/at/5?zoom=3", "<main><p>cell 2 5 3</p></main>"},
      {"/?tab=news", "<main><p>start news</p></main>"},
      // A parameter named from the path that is no such value matches no route, as one by rank.
      {"/tagged/12", "<main><section><p>tagged 12</p><p>tag 12</p></section></main>"},
      {"/tagged/abc?from=x", "<main><p>lost x</p></main>"},
      {"/nowhere", "<main><p>lost </p></main>"},
  };

  for (auto const &[start, shown_there] : pages) {
    virtual_time time;
    memory_renderer const shown;
    test_app instance{time.get_executor(), shown, shelf_root, {.contexts = {routing<shelf_routes>(start)}}};

    auto const act = []() -> task<> { co_await settle(); };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
    CHECK(shown.html() == shown_there);
  }
}

TEST_CASE("router: the address of a page has the fields its route names, and only those", "[ui][router]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shelf_root, {.contexts = {routing<shelf_routes>("/")}}};
  auto const &nav = instance.context<router>();

  // A layout's field named from the path is the page's to write, and a field no route names is left out.
  CHECK(nav.href(item_page, {.id = 3, .note = "kept out"}) == "/shelf/3");
  CHECK(nav.href(shelf, {.id = 3, .sort = "name"}) == "/shelf?sort=name");
  CHECK(nav.href(cell_page, {.row = 4, .col = 1}) == "/grid/4/at/1");
  CHECK(nav.href(cell_page, {.row = 4, .col = 1, .zoom = 2}) == "/grid/4/at/1?zoom=2");
  CHECK(nav.href(start_page, {.tab = "news"}) == "/?tab=news");
  CHECK(nav.href(start_page, {}) == "/");
}

TEST_CASE("router: a link goes to its page, which the history goes back from", "[ui][router]") {
  loaded.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shell, at("/")};
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
  test_app instance{time.get_executor(), shown, shell, at("/products/7")};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.context<router>().go(product_page, {.id = 8});
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
  test_app instance{time.get_executor(), shown, shell, at("/")};
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

TEST_CASE(
    "router: the address of a page, from its props, the fields its route names from the query in it", "[ui][router]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shell, at("/")};
  auto const &nav = instance.context<router>();

  CHECK(nav.href(product_page, {.id = 42}) == "/products/42");
  CHECK(nav.href(product_list, {.q = std::nullopt}) == "/products");
  CHECK(nav.href(product_list, {.q = "tea & more"}) == "/products?q=tea%20%26%20more");
  CHECK(nav.href(home_page) == "/");
  CHECK_THROWS_AS(nav.href(shell), std::logic_error);
}

TEST_CASE("router: a copy of a page, as a menu holds one, is the page its route names", "[ui][router]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shell, at("/")};
  auto const &nav = instance.context<router>();

  struct menu_item {
    component<product_page_props> page;
    component<> home;
  };

  menu_item const item{.page = product_page, .home = home_page};

  CHECK(nav.href(item.page, {.id = 7}) == "/products/7");
  CHECK(nav.href(item.home) == "/");
}

TEST_CASE("router: an address written without its first '/' is the same, pushed and matched", "[ui][router]") {
  // A history of the test's, which outlives the app, to read what it was given.
  memory_history history;
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shell, {.contexts = {routing<shop_routes>(history.ref())}}};

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
  test_app instance{time.get_executor(), shown, shell, at("/")};
  std::string href;

  auto const act = [&]() -> task<> {
    co_await settle();
    href = instance.context<router>().href(file_page, {.path = "/docs/report.txt"});
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
  test_app instance{time.get_executor(), shown, shell, {.contexts = {routing<shop_routes>(history.ref())}}};

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
  test_app instance{time.get_executor(), shown, shell, {.contexts = {routing<shop_routes>(history.ref())}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<router>().go("/products/5");
    instance.context<router>().go(product_page, {.id = 5});

    // One that redirects to it, as /items/5.
    instance.context<router>().go("/items/5");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(history.size() == 1);
}
