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
#include <quoll/ui/component/child_component.hpp>
#include <quoll/ui/component/hooks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

template <class C>
using test_app = app<C, memory_renderer>;

// A count, one more at each click.
element counter() {
  auto &count = use_state(0);
  auto const add = [&count] { count = *count + 1; };

  return button(std::to_string(*count)) | on_click(add);
}

struct counter_page {
  element view() { return component<counter>(); }
};

// What `auto x = use_state()` would be: a copy, which does not compile.
static_assert(!std::is_copy_constructible_v<std::remove_cvref_t<decltype(use_state(0))>>);

std::vector<std::string> loads;

task<std::string> load_text(int key) {
  loads.push_back("start " + std::to_string(key));
  co_await quoll::async::sleep_for(10ms);
  co_return "text " + std::to_string(key);
}

struct loader_props {
  int key = 0;

  [[nodiscard]] bool operator==(loader_props const &) const = default;
};

// Loads the text of the key its props give, as it changes.
element loader(loader_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_text, text);

  return p(text->loading ? std::string{"loading"} : text->value);
}

struct loader_page {
  state<int> key{1};

  element view() { return component<loader>({.key = *key}); }
};

// A context, its name changed from outside.
struct names {
  state<std::string> name{"tea"};
};

element name_shown() {
  auto &shown = use_context<names>();

  return p(*shown->name);
}

struct names_page {
  element view() { return component<name_shown>(); }
};

// A function calling hooks, called twice by one view: each call its own state.
element counted(std::string const &label) {
  auto &count = use_state(0);
  auto const add = [&count] { count = *count + 1; };

  return button(label + " " + std::to_string(*count)) | on_click(add);
}

element two_counters() { return div({counted("a"), counted("b")}); }

struct two_counters_page {
  element view() { return component<two_counters>(); }
};

// A state in a branch: kept while the branch is not taken.
element folding() {
  auto &open = use_state(true);
  auto const toggle = [&open] { open = !*open; };

  if (!*open) {
    return button("closed") | on_click(toggle);
  }

  auto &clicks = use_state(0);
  auto const click = [&clicks] { clicks = *clicks + 1; };

  return div({button("close") | on_click(toggle), button("clicks " + std::to_string(*clicks)) | on_click(click)});
}

struct folding_page {
  element view() { return component<folding>(); }
};

// A struct's view calling a hook: refused.
struct hooking_struct {
  element view() {
    static_cast<void>(use_state(1));
    return p("never");
  }
};

// Loads the sum of two keys, as either changes, and runs once a function of nothing as it first shows.
std::vector<std::string> ran;

task<std::string> load_sum(int left, int right) {
  ran.push_back("sum " + std::to_string(left + right));
  co_await quoll::async::sleep_for(1ms);
  co_return std::to_string(left + right);
}

task<std::string> load_once() {
  ran.push_back("once");
  co_return "once";
}

struct pair_props {
  int left = 0;
  int right = 0;

  [[nodiscard]] bool operator==(pair_props const &) const = default;
};

element pair_loader(pair_props const &given) {
  auto &sum = use_async_state<std::string>();
  auto &once = use_async_state<std::string>();

  use_effect(std::tuple{given.left, given.right}, load_sum, sum);
  use_effect(load_once, once);

  return p(sum->value + " " + once->value);
}

struct pair_page {
  state<int> left{1};
  state<int> right{2};

  element view() { return component<pair_loader>({.left = *left, .right = *right}); }
};

// The function an effect runs is the one of the last view: what it captures is never stale.
std::vector<std::string> said;

struct greeter_props {
  int key = 0;
  std::string word;

  [[nodiscard]] bool operator==(greeter_props const &) const = default;
};

element greeter(greeter_props const &given) {
  use_effect(given.key, [word = given.word](int key) -> task<> {
    said.push_back(word + " " + std::to_string(key));
    co_return;
  });

  return p(given.word);
}

struct greeter_page {
  state<int> key{1};
  state<std::string> word{"hello"};

  element view() { return component<greeter>({.key = *key, .word = *word}); }
};

// An action of a free function, and one of a member function of a context.
std::vector<int> doubled_by;

task<int> doubled(int value) {
  doubled_by.push_back(value);
  co_await quoll::async::sleep_for(1ms);
  co_return value * 2;
}

struct counter_store {
  state<int> total{0};

  task<int> add(int amount) {
    co_await quoll::async::sleep_for(1ms);
    total = *total + amount;
    co_return *total;
  }
};

element actions() {
  auto &store = use_context<counter_store>();
  auto &twice = use_action<&doubled>();
  auto &add = use_action<&counter_store::add>(store);

  return div({
      button("twice " + std::to_string(twice->value)) | on_click(twice.with(21)),
      button("total " + std::to_string(*store->total)) | on_click(add.with(5)),
  });
}

struct actions_page {
  element view() { return component<actions>(); }
};

// A form of what the props give, saved by an action with the form as it stands.
struct item {
  std::string name;
  int stock = 0;

  [[nodiscard]] bool operator==(item const &) const = default;
};

std::vector<item> saved_items;

task<item> save_item(item edited) {
  saved_items.push_back(edited);
  co_return edited;
}

struct item_props {
  item value;

  [[nodiscard]] bool operator==(item_props const &) const = default;
};

element item_editor(item_props const &given) {
  auto &form = use_form(given.value);
  auto &save = use_action<&save_item>();

  return div({
      label({text("Stock"), input() | bind(form, &item::stock)}),
      when(!form.valid(), [] { return p("not a number"); }),
      button("Save") | disabled(!form.valid()) | on_click(save.with(form)),
  });
}

struct item_page {
  element view() { return component<item_editor>({.value = {.name = "tea", .stock = 3}}); }
};

// A context provided to the components of a view, found before the app's.
struct theme {
  state<std::string> name{"light"};
};

element theme_shown() { return p(*use_context<theme>()->name); }

element dark_area() {
  auto &own = provide_context<theme>();

  // Set once: a state set at each view would ask for another without end.
  if (*own->name != "dark") {
    own->name.set("dark");
  }
  return div({component<theme_shown>()});
}

struct themes_page {
  element view() { return div({component<theme_shown>(), component<dark_area>()}); }
};

// A struct function component: its props and helpers together, its state its hooks.
struct badge {
  struct props {
    std::string label;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element operator()(props const &given) const {
    auto &clicks = use_state(0);
    auto const add = [&clicks] { clicks = *clicks + 1; };

    return button(shown(given, *clicks)) | on_click(add);
  }

  [[nodiscard]] static std::string shown(props const &given, int clicks) {
    return given.label + " " + std::to_string(clicks);
  }
};

struct badge_page {
  element view() { return component<badge>({.label = "badge"}); }
};

// A place whose hook changes type: refused.
element mismatched() {
  auto &flip = use_state(false);
  auto const toggle = [&flip] { flip = !*flip; };
  auto const here = std::source_location::current();

  if (*flip) {
    static_cast<void>(use_state(std::string{}, here));
  } else {
    static_cast<void>(use_state(0, here));
  }

  return button("flip") | on_click(toggle);
}

struct mismatched_page {
  element view() { return component<mismatched>(); }
};

// A context nobody gave.
struct never_given {};

element missing_context() {
  static_cast<void>(use_context<never_given>());
  return p("never");
}

struct missing_page {
  element view() { return component<missing_context>(); }
};

// An effect stopped as its component goes, and one debounced.
std::vector<std::string> lasting;

task<std::string> load_slow(int key) {
  lasting.push_back("start " + std::to_string(key));
  co_await quoll::async::sleep_for(1min);
  lasting.push_back("end " + std::to_string(key));
  co_return "slow";
}

struct slow_props {
  int key = 0;

  [[nodiscard]] bool operator==(slow_props const &) const = default;
};

element slow_loader(slow_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_slow, text);
  return p("slow");
}

struct going_page {
  state<bool> shows{true};

  element view() { return *shows ? component<slow_loader>({.key = 1}) : p("gone"); }
};

element debounced_loader(loader_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_text, text, {.debounce = 50ms});
  return p(text->value);
}

struct debounced_page {
  state<int> key{1};

  element view() { return component<debounced_loader>({.key = *key}); }
};

// A struct function component of no props.
struct plain_badge {
  element operator()() const {
    auto &clicks = use_state(0);

    return p(std::to_string(*clicks));
  }
};

struct plain_badge_page {
  element view() { return component<plain_badge>(); }
};

// A function calling hooks called in a branch: the later calls of that place shift, as documented.
element conditional_counters() {
  auto &first_shown = use_state(true);
  auto const hide = [&first_shown] { first_shown = false; };

  return div({
      when(*first_shown, [] { return counted("a"); }),
      counted("b"),
      button("hide") | on_click(hide),
  });
}

struct conditional_page {
  element view() { return component<conditional_counters>(); }
};

// An effect capturing `given` by reference, which its run keeps, read after the props changed.
std::vector<std::string> read_after;

struct named_props {
  int key = 0;
  std::string name;

  [[nodiscard]] bool operator==(named_props const &) const = default;
};

element named_loader(named_props const &given) {
  use_effect(given.key, [&given](int) -> task<> {
    co_await quoll::async::sleep_for(10ms);
    read_after.push_back(given.name);
  });

  return p(given.name);
}

struct named_page {
  state<int> key{1};
  state<std::string> name{"a name long enough to be on the heap, not in a short string"};

  element view() { return component<named_loader>({.key = *key, .name = *name}); }
};

// A null function given to an effect.
element null_effect() {
  task<std::string> (*load)(int) = nullptr;
  auto &text = use_async_state<std::string>();

  use_effect(1, load, text);
  return p("never");
}

struct null_effect_page {
  element view() { return component<null_effect>(); }
};

} // namespace

TEST_CASE("function component: a state of its view, the same at each view", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<counter_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<button>2</button>");
}

TEST_CASE(
    "function component: an effect on a value of its props, its target loading from its first view", "[ui][function]"
) {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<loader_page> instance{time.get_executor(), shown};
  std::string first;
  std::string after_change;

  auto const act = [&]() -> task<> {
    first = shown.html();
    co_await quoll::async::sleep_for(20ms);
    instance.component().key = 2;
    co_await settle();
    after_change = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first == "<p>loading</p>");
  CHECK(after_change == "<p>loading</p>");
  CHECK(shown.html() == "<p>text 2</p>");
  CHECK(loads == std::vector<std::string>{"start 1", "start 2"});
}

TEST_CASE(
    "function component: a context of its app, read through use_context, shows it again as it changes", "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app<names_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};
  std::string before;

  auto const act = [&]() -> task<> {
    co_await settle();
    before = shown.html();
    instance.context<names>().name = "mate";
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before == "<p>tea</p>");
  CHECK(shown.html() == "<p>mate</p>");
}

TEST_CASE("function component: a function calling hooks, called twice in a view, has two of each", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<two_counters_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find_text("b 0"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><button>a 0</button><button>b 1</button></div>");
}

TEST_CASE("function component: a state in a branch not taken keeps what it holds", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<folding_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find_text("clicks 0"), "click");
    co_await settle();
    instance.dispatch(shown.find_text("close"), "click");
    co_await settle();
    instance.dispatch(shown.find_text("closed"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("clicks 1"));
}

TEST_CASE("function component: a hook called outside the view of one is refused", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<hooking_struct> instance{time.get_executor(), shown};

  CHECK_THROWS_AS(instance.start(), std::logic_error);
}

TEST_CASE("function component: an effect on several values, and one on none that runs once", "[ui][function]") {
  ran.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<pair_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().right = 5;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(ran == std::vector<std::string>{"sum 3", "once", "sum 6"});
  CHECK(shown.html() == "<p>6 once</p>");
}

TEST_CASE("function component: the function an effect runs is the one of the last view", "[ui][function]") {
  said.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<greeter_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().word = "bye";
    co_await settle();
    instance.component().key = 2;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(said == std::vector<std::string>{"hello 1", "bye 2"});
}

TEST_CASE(
    "function component: an action of a free function, and one of a context's member function", "[ui][function]"
) {
  doubled_by.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<actions_page> instance{time.get_executor(), shown, {.contexts = {provide<counter_store>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find_text("twice 0"), "click");
    instance.dispatch(shown.find_text("total 0"), "click");
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(doubled_by == std::vector<int>{21});
  CHECK(*instance.context<counter_store>().total == 5);
  CHECK(shown.html() == "<div><button>twice 42</button><button>total 5</button></div>");
}

TEST_CASE(
    "function component: a form of what its props give, saved as it stands, refused while wrong", "[ui][function]"
) {
  saved_items.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<item_page> instance{time.get_executor(), shown};
  bool told_wrong = false;

  auto const act = [&]() -> task<> {
    co_await settle();
    auto const stock = shown.children(shown.find_text("Stock")).at(1);

    instance.dispatch(stock, "input", "x");
    co_await settle();
    told_wrong = shown.html().contains("not a number");
    instance.dispatch(shown.find_text("Save"), "click");
    co_await settle();
    instance.dispatch(stock, "input", "8");
    co_await settle();
    instance.dispatch(shown.find_text("Save"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(told_wrong);
  CHECK(saved_items == std::vector<item>{{.name = "tea", .stock = 8}});
}

TEST_CASE(
    "function component: a context it provides is found by its view's components, before the app's", "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app<themes_page> instance{time.get_executor(), shown, {.contexts = {provide<theme>()}}};

  auto const act = [&]() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><p>light</p><div><p>dark</p></div></div>");
}

TEST_CASE("function component: a struct whose operator() gives the view, its state its hooks", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<badge_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<button>badge 1</button>");
}

TEST_CASE("function component: a hook of another type where one was called before is refused", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app<mismatched_page> instance{time.get_executor(), shown, {.on_error = [&failures](std::exception_ptr failure) {
                                       try {
                                         std::rethrow_exception(failure);
                                       } catch (std::exception const &error) {
                                         failures.emplace_back(error.what());
                                       }
                                     }}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(failures.empty());
  CHECK(failures.front() == "quoll::ui: a use_ hook of another type where one was called before");
}

TEST_CASE("function component: a context none provides is refused", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<missing_page> instance{time.get_executor(), shown};

  CHECK_THROWS_AS(instance.start(), std::logic_error);
}

TEST_CASE("function component: an effect stops as its component goes", "[ui][function]") {
  lasting.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<going_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(lasting == std::vector<std::string>{"start 1"});
}

TEST_CASE("function component: an effect's options, as a debounce, given to use_effect", "[ui][function]") {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<debounced_page> instance{time.get_executor(), shown};
  std::vector<std::string> waiting;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(20ms);
    instance.component().key = 2;
    co_await quoll::async::sleep_for(20ms);
    instance.component().key = 3;
    co_await quoll::async::sleep_for(40ms);
    waiting = loads;
    co_await quoll::async::sleep_for(30ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(waiting == std::vector<std::string>{"start 1"});
  CHECK(loads == std::vector<std::string>{"start 1", "start 3"});
}

TEST_CASE("function component: a struct one of no props", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<plain_badge_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>0</p>");
}

TEST_CASE(
    "function component: a function calling hooks called in a branch shifts the later calls of its place",
    "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app<conditional_page> instance{time.get_executor(), shown};
  std::string before;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find_text("a 0"), "click");
    co_await settle();
    before = shown.html();
    instance.dispatch(shown.find_text("hide"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before.contains("a 1"));
  CHECK(before.contains("b 0"));
  // b is now the first call of that place, and takes the state a had: as React's hooks, by rank.
  CHECK(shown.html().contains("b 1"));
}

TEST_CASE(
    "function component: an effect capturing its props reads them after they changed, as its run keeps them",
    "[ui][function]"
) {
  read_after.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<named_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    // The props change while the run waits: it reads those of the view that started it.
    instance.component().name = "another name long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(read_after == std::vector<std::string>{"a name long enough to be on the heap, not in a short string"});
}

TEST_CASE("function component: an effect given no function is refused", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<null_effect_page> instance{time.get_executor(), shown};

  CHECK_THROWS_AS(instance.start(), std::invalid_argument);
}
