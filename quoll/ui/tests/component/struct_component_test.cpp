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
#include <quoll/ui/detail/context/member_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
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

std::vector<std::int64_t> loads;

// A later id loads faster: the load of an id left behind would end last, and show, unless stopped.
task<std::string> load_for(std::int64_t id) {
  loads.push_back(id);
  co_await quoll::async::sleep_for(std::chrono::milliseconds{10 - id});
  co_return "loaded " + std::to_string(id);
}

std::vector<std::string> followed;

task<> note_followed(std::int64_t id) {
  followed.push_back("id " + std::to_string(id));
  co_return;
}

// What it loads follows a prop: read from the props of each view, the load before stopped as it changes.
struct product_card : component<product_card> {
  struct props {
    std::int64_t id = 0;
    std::string note;

    bool operator==(props const &) const = default;
  };

  async_state<std::string> loaded;
  effect load{&props::id, load_for, loaded};
  effect note{&props::id, note_followed};

  element view() { return p(loaded->loading ? std::string{"loading"} : loaded->value + " " + given->note); }
};

inline constexpr tag<product_card> product_card;

struct product_card_data {
  state<std::int64_t> id{1};
  state<std::string> note{"a"};
};

struct product_card_page : component<product_card_page> {
  element view() {
    auto &self = use_context<product_card_data>();

    return product_card({.id = *self->id, .note = *self->note});
  }
};

inline constexpr tag<product_card_page> product_card_page;

// What a function of its props gives: a nested field, a field of a base, a const one.
struct sized {
  std::int64_t size = 0;

  bool operator==(sized const &) const = default;
};

struct shelf_item {
  std::int64_t id = 0;
};

struct shelf_card : component<shelf_card> {
  struct props : sized {
    shelf_item item;
    std::int64_t const rank = 0;

    bool operator==(props const &other) const { return item.id == other.item.id && size == other.size; }
  };

  async_state<std::string> by_item;
  async_state<std::string> by_size;
  async_state<std::string> by_rank;
  effect item_load{[](props const &given) noexcept { return given.item.id; }, load_for, by_item};
  effect size_load{[](props given) static { return given.size; }, load_for, by_size};
  effect rank_load{&props::rank, load_for, by_rank};

  element view() { return p(by_item->value + ", " + by_size->value + ", " + by_rank->value); }
};

inline constexpr tag<shelf_card> shelf_card;

struct shelf_data {
  state<std::int64_t> id{1};
  state<std::int64_t> size{2};
  state<std::int64_t> rank{3};
};

struct shelf_page : component<shelf_page> {
  element view() {
    auto &self = use_context<shelf_data>();

    return shelf_card({{.size = *self->size}, {.id = *self->id}, *self->rank});
  }
};

inline constexpr tag<shelf_page> shelf_page;

// An effect following the props of another struct: refused as it is made.
struct mismatched : component<mismatched> {
  struct props {
    std::int64_t id = 0;

    bool operator==(props const &) const = default;
  };

  async_state<std::string> loaded;
  effect load{&product_card::props::id, load_for, loaded};

  element view() { return p("never"); }
};

inline constexpr tag<mismatched> mismatched;

struct mismatched_page : component<mismatched_page> {
  element view() { return mismatched({.id = 1}); }
};

inline constexpr tag<mismatched_page> mismatched_page;

// What follows its props throws, as a copy lacking memory would: reported after the view, rather than end the program.
std::int64_t throwing_id(std::int64_t /*id*/) { throw std::runtime_error{"no id"}; }

struct throwing_follow : component<throwing_follow> {
  struct props {
    std::int64_t id = 0;

    bool operator==(props const &) const = default;
  };

  async_state<std::string> loaded;
  // An optional, which may hold none: read before the first view, where nothing may throw.
  effect load{[](props const &given) { return std::optional{throwing_id(given.id)}; }, load_for, loaded};

  element view() { return p(loaded->error ? std::string{"failed"} : loaded->loading ? "loading" : "idle"); }
};

inline constexpr tag<throwing_follow> throwing_follow;

struct throwing_follow_page : component<throwing_follow_page> {
  element view() { return throwing_follow({.id = 1}); }
};

inline constexpr tag<throwing_follow_page> throwing_follow_page;

// What it follows may hold none, it is debounced, and its button loads again.
struct waited_card : component<waited_card> {
  struct props {
    std::optional<std::int64_t> id;

    bool operator==(props const &) const = default;
  };

  async_state<std::string> loaded;
  effect load{&props::id, load_for, loaded, {.debounce = 5ms}};

  element view() {
    return button(loaded->loading ? std::string{"loading"} : loaded->value) | on_click([this] { load.reload(); });
  }
};

inline constexpr tag<waited_card> waited_card;

struct waited_data {
  state<std::optional<std::int64_t>> id;
};

struct waited_page : component<waited_page> {
  element view() {
    auto &self = use_context<waited_data>();

    return waited_card({.id = *self->id});
  }
};

inline constexpr tag<waited_page> waited_page;

// What does not say whose props it follows, or what it gives of them, is refused.
using some_target = state<std::string>;

static_assert(!std::constructible_from<
              effect, decltype([](auto const &given) { return given.id; }), decltype(&load_for), some_target &>);
static_assert(!std::constructible_from<
              effect, decltype([](product_card::props &given) { return given.id; }), decltype(&load_for),
              some_target &>);
static_assert(!std::constructible_from<effect, decltype([](product_card::props const &) {}), decltype(&note_followed)>);
static_assert(!std::constructible_from<
              effect, std::int64_t (product_card::props::*)() const, decltype(&load_for), some_target &>);

// A field a base of its props declares, named by a member pointer: that of the base, refused as another's.
struct base_field : component<base_field> {
  struct props : sized {
    bool operator==(props const &) const = default;
  };

  async_state<std::string> loaded;
  effect load{&props::size, load_for, loaded};

  element view() { return p("never"); }
};

inline constexpr tag<base_field> base_field;

struct base_field_page : component<base_field_page> {
  element view() { return base_field({{.size = 1}}); }
};

inline constexpr tag<base_field_page> base_field_page;

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

namespace {

// A member of the registry below, which may make another go as its effects start.
struct probe {
  std::string name;
  std::vector<std::string> *calls = nullptr;
  quoll::ui::detail::member_registry *registry = nullptr;
  std::unique_ptr<probe> *going = nullptr;
};

void probe_after_view(void *object, quoll::ui::detail::effect_starter const & /*start*/) {
  auto &self = *static_cast<probe *>(object);

  self.calls->push_back(self.name);

  // As an on_error resetting an optional of the struct would.
  if (self.going != nullptr && *self.going) {
    self.registry->remove(self.going->get());
    self.going->reset();
  }
}

} // namespace

TEST_CASE(
    "struct component: a member that goes as the effects start is not called, and the others are, each once",
    "[ui][struct]"
) {
  quoll::ui::detail::member_registry registry;
  std::vector<std::string> calls;
  auto first = std::make_unique<probe>(probe{.name = "first", .calls = &calls});
  auto third = std::make_unique<probe>(probe{.name = "third", .calls = &calls});
  probe second{.name = "second", .calls = &calls, .registry = &registry, .going = &first};
  probe fourth{.name = "fourth", .calls = &calls, .registry = &registry, .going = &third};

  // Each joins by its own address, as a member_join would.
  for (auto *const member : {first.get(), &second, third.get(), &fourth}) {
    registry.add({.object = member, .join = member, .after_view = &probe_after_view});
  }

  // The second makes the first go, behind it, and the fourth the third, behind it too: none after them is skipped.
  registry.run_effects({});
  CHECK(calls == std::vector<std::string>{"first", "second", "third", "fourth"});

  // Gone for good once they started.
  calls.clear();
  registry.run_effects({});
  CHECK(calls == std::vector<std::string>{"second", "fourth"});
}

TEST_CASE("struct component: a member that goes ahead of those starting is not called", "[ui][struct]") {
  quoll::ui::detail::member_registry registry;
  std::vector<std::string> calls;
  auto second = std::make_unique<probe>(probe{.name = "second", .calls = &calls});
  probe third{.name = "third", .calls = &calls};
  probe first{.name = "first", .calls = &calls, .registry = &registry, .going = &second};

  for (auto *const member : {&first, second.get(), &third}) {
    registry.add({.object = member, .join = member, .after_view = &probe_after_view});
  }

  registry.run_effects({});
  CHECK(calls == std::vector<std::string>{"first", "third"});
}

TEST_CASE(
    "struct component: an effect on a prop runs as it is first viewed, and again only as that prop changes",
    "[ui][struct]"
) {
  loads.clear();
  followed.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, product_card_page, {.contexts = {provide<product_card_data>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<p>loaded 1 a</p>");

    // Another prop changed: no load.
    instance.context<product_card_data>().note = "b";
    co_await quoll::async::sleep_for(10ms);
    CHECK(loads == std::vector<std::int64_t>{1});

    // Its prop changed twice at once: the load of 2, slower, stopped, rather than end after that of 3 and show.
    instance.context<product_card_data>().id = 2;
    co_await settle();
    instance.context<product_card_data>().id = 3;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>loaded 3 b</p>");
  CHECK(loads == std::vector<std::int64_t>{1, 2, 3});
  CHECK(followed == std::vector<std::string>{"id 1", "id 2", "id 3"});
}

TEST_CASE(
    "struct component: an effect on a prop outside the making of a struct, or on none, is refused", "[ui][struct]"
) {
  state<std::string> target;

  using Catch::Matchers::ContainsSubstring;

  CHECK_THROWS_WITH(
      (effect{&product_card::props::id, load_for, target}), ContainsSubstring("is a member of a component written")
  );
  CHECK_THROWS_WITH(
      (effect{static_cast<std::int64_t product_card::props::*>(nullptr), load_for, target}),
      ContainsSubstring("no prop")
  );
  CHECK_THROWS_WITH(
      (effect{static_cast<std::int64_t (*)(product_card::props const &)>(nullptr), load_for, target}),
      ContainsSubstring("no prop")
  );
}

TEST_CASE(
    "struct component: an effect follows a function of its props, a nested field, a base's, a const one", "[ui][struct]"
) {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, shelf_page, {.contexts = {provide<shelf_data>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(20ms);
    CHECK(shown.html() == "<p>loaded 1, loaded 2, loaded 3</p>");
    CHECK(loads == std::vector<std::int64_t>{1, 2, 3});

    // The nested field changed: its load alone, through a function of the props.
    instance.context<shelf_data>().id = 4;
    co_await quoll::async::sleep_for(20ms);
    CHECK(shown.html() == "<p>loaded 4, loaded 2, loaded 3</p>");
    CHECK(loads == std::vector<std::int64_t>{1, 2, 3, 4});

    // A field the props' == leaves out: they compare equal, the last view is given again, and no effect sees it.
    instance.context<shelf_data>().rank = 5;
    co_await quoll::async::sleep_for(20ms);
    CHECK(loads == std::vector<std::int64_t>{1, 2, 3, 4});

    // The base's field changed: its load, and that of the field left out, seen with these props.
    instance.context<shelf_data>().size = 6;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>loaded 4, loaded 6, loaded 5</p>");
  CHECK(loads == std::vector<std::int64_t>{1, 2, 3, 4, 6, 5});
}

TEST_CASE("struct component: an effect on the props of another struct is refused as it is made", "[ui][struct]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, mismatched_page};

  auto const act = [&]() -> task<int> {
    CHECK_THROWS_WITH(instance.start(), Catch::Matchers::ContainsSubstring("follows its own component's props"));
    co_return 0;
  };

  CHECK(quoll::test::run(time, act()) == 0);
}

TEST_CASE(
    "struct component: an effect on a base's field by a member pointer is refused as it is made", "[ui][struct]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, base_field_page};

  auto const act = [&]() -> task<int> {
    CHECK_THROWS_WITH(instance.start(), Catch::Matchers::ContainsSubstring("a field a base of them declares"));
    co_return 0;
  };

  CHECK(quoll::test::run(time, act()) == 0);
}

TEST_CASE("struct component: what follows a prop that throws is reported after the view", "[ui][struct]") {
  std::vector<std::string> errors;
  virtual_time time;
  memory_renderer const shown;
  test_app instance{
      time.get_executor(),
      shown,
      throwing_follow_page,
      {.on_error =
           [&errors](std::exception_ptr const &error) {
             try {
               std::rethrow_exception(error);
             } catch (std::exception const &failure) {
               errors.emplace_back(failure.what());
             }
           }}
  };

  auto const act = []() -> task<> { co_await quoll::async::sleep_for(10ms); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>failed</p>");
  CHECK(errors == std::vector<std::string>{"no id"});
}

TEST_CASE("struct component: an effect on a prop holding none waits, debounced, and loads again", "[ui][struct]") {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, waited_page, {.contexts = {provide<waited_data>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(20ms);
    CHECK(shown.html() == "<button></button>");
    CHECK(loads.empty());

    // The first run that starts does not wait.
    instance.context<waited_data>().id = 1;
    co_await quoll::test::settle();
    CHECK(loads == std::vector<std::int64_t>{1});
    co_await quoll::async::sleep_for(20ms);
    CHECK(shown.html() == "<button>loaded 1</button>");

    // A change waits for the pause, loading meanwhile.
    instance.context<waited_data>().id = 2;
    co_await quoll::async::sleep_for(2ms);
    CHECK(shown.html() == "<button>loading</button>");
    CHECK(loads == std::vector<std::int64_t>{1});
    co_await quoll::async::sleep_for(20ms);
    CHECK(shown.html() == "<button>loaded 2</button>");

    // reload() reads the props again, at once.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::test::settle();
    CHECK(loads == std::vector<std::int64_t>{1, 2, 2});
    co_await quoll::async::sleep_for(20ms);

    // None again: the target loads no more, its value kept.
    instance.context<waited_data>().id = std::nullopt;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<button>loaded 2</button>");
  CHECK(loads == std::vector<std::int64_t>{1, 2, 2});
}
