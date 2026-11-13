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

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace struct_component_test {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

int tally_views = 0;

// A count, one more at each click, written as a struct: its props, its state, its handler, its view.
struct tally : component<tally> {
  struct props {
    std::string label;

    bool operator==(props const &) const = default;
  };

  // State
  state<int> count{0};

  // Handlers
  void add() { count = *count + 1; }

  // View
  element view() {
    ++tally_views;

    return button(given->label + ":" + std::to_string(*count)) | on_click([this] { add(); });
  }
};

inline constexpr tag<tally> tally;

// What the page shows, a context of the app.
struct tally_page_data {
  state<std::string> label{"a"};
  state<int> other{0};
};

// The root, a struct with no props, reading a context through use_context() in its view.
struct tally_page : component<tally_page> {
  element view() {
    auto &self = use_context<tally_page_data>();

    return div({tally({.label = *self->label}), p(std::to_string(*self->other))});
  }
};

inline constexpr tag<tally_page> tally_page;

task<std::string> find_text(std::string query) {
  co_await quoll::async::sleep_for(5ms);
  co_return "found " + query;
}

// A search, its effect a member following a state, writing into another.
struct search_box : component<search_box> {
  state<std::string> query{"tea"};
  async_state<std::string> found;
  effect search{query, find_text, found};

  element view() {
    auto const change = [this] { query = "coffee"; };

    return div({p(found->loading ? std::string{"loading"} : found->value), button("coffee") | on_click(change)});
  }
};

inline constexpr tag<search_box> search_box;

// A context its member finds as it is made, and that it shows as it changes.
struct names {
  state<std::vector<std::string>> all{std::vector<std::string>{"tea"}};
};

struct name_list : component<name_list> {
  shared<names> from;

  element view() {
    auto const add = [this] { from->all.update([](auto &held) { held.push_back("coffee"); }); };

    return button(std::to_string(from->all->size())) | on_click(add);
  }
};

inline constexpr tag<name_list> name_list;

// One using a context no app is given: refused as it is made.
struct never_given {};

struct orphan : component<orphan> {
  shared<never_given> missing;

  element view() { return p("never"); }
};

inline constexpr tag<orphan> orphan;

// Pages written as structs, named by their variables in the routes.
struct item_page : component<item_page> {
  struct props {
    std::int64_t id = 0;

    bool operator==(props const &) const = default;
  };

  element view() { return p("item " + std::to_string(given->id)); }
};

inline constexpr tag<item_page> item_page;

struct home_page : component<home_page> {
  element view() { return div({link_to(item_page, {.id = 3}, "three")}); }
};

inline constexpr tag<home_page> home_page;

struct shell : component<shell> {
  element view() { return main_({outlet()}); }
};

inline constexpr tag<shell> shell;

inline constexpr auto item_routes =
    quoll::ui::routes(route<"/", home_page>(), route<"/items/{id}", item_page, &item_page::props::id>());

// What an app's on_error is given, in words.
[[nodiscard]] auto collect(std::vector<std::string> &failures) {
  return [&failures](std::exception_ptr failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      failures.emplace_back(error.what());
    }
  };
}

// A form, a member, taking the value its props give; a state made from its first props.
struct item {
  std::string name;

  bool operator==(item const &) const = default;
};

struct item_form : component<item_form> {
  struct props {
    item value;
    std::string label;

    bool operator==(props const &) const = default;
  };

  form_state<item> form;
  state<std::string> first_label{given->label};

  element view() {
    form.take(given->value);

    return div({input() | bind(form, &item::name), p(form.value().name + " " + *first_label + " " + given->label)});
  }
};

inline constexpr tag<item_form> item_form;

struct item_form_data {
  state<std::string> label{"first"};
};

struct item_form_page : component<item_form_page> {
  element view() {
    auto &self = use_context<item_form_data>();

    return item_form({.value = {.name = "tea"}, .label = *self->label});
  }
};

inline constexpr tag<item_form_page> item_form_page;

task<int> echo_count(int value) { co_return value; }

// An effect in an optional, which goes before the struct: it leaves what it joined, and runs no more.
struct passing : component<passing> {
  state<int> kept{0};
  state<int> seen{0};
  std::optional<effect> watch{std::in_place, kept, echo_count, seen};

  element view() {
    auto const drop = [this] {
      watch.reset();
      kept = *kept + 1;
    };

    return button(std::to_string(*kept) + " " + std::to_string(*seen)) | on_click(drop);
  }
};

inline constexpr tag<passing> passing;

// A struct providing a context to the components of its view, from its view.
struct greeting {
  state<std::string> word{"hello"};
};

constexpr component<> greeter = [] {
  auto &said = use_context<greeting>();

  return p(*said->word);
};

struct greeting_host : component<greeting_host> {
  element view() {
    static_cast<void>(provide_context<greeting>());

    return div({greeter()});
  }
};

inline constexpr tag<greeting_host> greeting_host;

// What its handler read.
std::vector<std::string> clicks;

// A view that throws for some props: its handlers keep reading those of its last view that showed.
struct fragile : component<fragile> {
  struct props {
    std::string label;

    bool operator==(props const &) const = default;
  };

  element view() {
    if (given->label == "boom") {
      throw std::runtime_error{"boom"};
    }

    return button(given->label) | on_click([this] { clicks.push_back(given->label); });
  }
};

inline constexpr tag<fragile> fragile;

struct fragile_data {
  state<std::string> label{"calm"};
};

struct fragile_page : component<fragile_page> {
  element view() {
    auto &self = use_context<fragile_data>();

    return fragile({.label = *self->label});
  }
};

inline constexpr tag<fragile_page> fragile_page;

} // namespace struct_component_test

using namespace struct_component_test;

TEST_CASE(
    "struct component: its state a member, kept as its props change, not viewed again while equal", "[ui][struct]"
) {
  tally_views = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, tally_page, {.contexts = {provide<tally_page_data>()}}};
  int views_before = 0;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // Its props unchanged: the count is not viewed again.
    views_before = tally_views;
    instance.context<tally_page_data>().other = 1;
    co_await settle();
    CHECK(tally_views == views_before);

    // Its props changed: its count kept.
    instance.context<tally_page_data>().label = "b";
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><button>b:1</button><p>1</p></div>");
}

TEST_CASE(
    "struct component: an effect member runs as it is first viewed, and as the state it follows changes", "[ui][struct]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, search_box};
  std::string first;

  auto const act = [&]() -> task<> {
    co_await settle();
    first = shown.html();
    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html().contains("found tea"));

    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(10ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first.contains("<p>loading</p>"));
  CHECK(shown.html().contains("found coffee"));
}

TEST_CASE(
    "struct component: a context member is found as it is made, and shows it again as it changes", "[ui][struct]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, name_list, {.contexts = {provide<names>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<button>2</button>");
}

TEST_CASE("struct component: one using a context its app was not given is refused as it is made", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, orphan};

  auto const act = [&]() -> task<int> {
    try {
      instance.start();
    } catch (std::logic_error const &) {
      co_return 0;
    }

    co_return 1;
  };

  CHECK(quoll::test::run(time, act()) == 0);
}

TEST_CASE("struct component: pages written as structs have routes, links and addresses", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shell, {.contexts = {routing<item_routes>("/")}}};
  auto &nav = instance.context<router>();

  auto const act = [&]() -> task<> {
    co_await settle();
    CHECK(shown.html() == "<main><div><a data-quoll-link=\"\" href=\"/items/3\">three</a></div></main>");

    instance.dispatch(shown.find("a"), "click");
    co_await settle();
    CHECK(shown.html() == "<main><p>item 3</p></main>");

    nav.go(item_page, {.id = 4});
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<main><p>item 4</p></main>");
  CHECK(nav.href(item_page, {.id = 9}) == "/items/9");
  CHECK(nav.href(home_page) == "/");
}

TEST_CASE(
    "struct component: a form a member, a state made from its first props, both kept as its props change",
    "[ui][struct]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, item_form_page, {.contexts = {provide<item_form_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("input"), "input", "green tea");
    co_await settle();
    instance.context<item_form_data>().label = "second";
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>green tea first second</p>"));
}

TEST_CASE("struct component: a member that goes before it leaves what it joined", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, passing};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  // The view after the drop runs the struct's effects: one gone would run, under the sanitizers a use of freed memory.
  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<button>1 0</button>");
}

TEST_CASE("struct component: its view provides a context to the components of its view", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, greeting_host};

  auto const act = []() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><p>hello</p></div>");
}

TEST_CASE("struct component: a view that throws leaves its handlers the props of its last view", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;

  clicks.clear();

  test_app instance{
      time.get_executor(), shown, fragile_page, {.on_error = collect(failures), .contexts = {provide<fragile_data>()}}
  };

  auto const act = [&]() -> task<> {
    co_await settle();

    // Its next view throws, its props then gone; its handler reads those that showed.
    instance.context<fragile_data>().label = "boom";
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failures == std::vector<std::string>{"boom"});
  CHECK(clicks == std::vector<std::string>{"calm"});
}
