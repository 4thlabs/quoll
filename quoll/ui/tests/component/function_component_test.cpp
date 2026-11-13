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

using test_app = app<memory_renderer>;

// A count, one more at each click.
constexpr component<> counter = [] {
  auto &count = use_state(0);

  auto const add = [&count] { count = *count + 1; };

  return button(std::to_string(*count)) | on_click(add);
};

constexpr component<> counter_page = [] { return counter(); };

// The two models of CommittedHook: what a view writes takes effect once it shows.
task<int> committed_model(int value) { co_return value; }

static_assert(detail::CommittedHook<detail::effect_hook<task<int> (*)(int), int>>);
static_assert(detail::CommittedHook<detail::action_hook<&committed_model, overlap::refuse>>);

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
constexpr component<loader_props> loader = [](loader_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_text, text);

  return p(text->loading ? std::string{"loading"} : text->value);
};

// What the test changes of loader_page.
struct loader_host {
  state<int> key{1};
};

constexpr component<> loader_page = [] {
  auto &host = use_context<loader_host>();

  return loader({.key = *host->key});
};

// An input bound to a state of a string, and one of a bool: bind() is Quoll's, not std::bind, which the string brings.
constexpr component<> typing = [] {
  auto &text = use_state<std::string>();
  auto &ticked = use_state(false);

  return div({input() | bind(text), input() | bind(ticked), p(*text + (*ticked ? " ticked" : ""))});
};

// A context, its name changed from outside.
struct names {
  state<std::string> name{"tea"};
};

constexpr component<> name_shown = [] {
  auto &shown = use_context<names>();

  return p(*shown->name);
};

constexpr component<> names_page = [] { return name_shown(); };

// A function calling hooks, called twice by one view: each call its own state.
element counted(std::string const &label) {
  auto &count = use_state(0);

  auto const add = [&count] { count = *count + 1; };

  return button(label + " " + std::to_string(*count)) | on_click(add);
}

constexpr component<> two_counters = [] { return div({counted("a"), counted("b")}); };

constexpr component<> two_counters_page = [] { return two_counters(); };

// A state in a branch: kept while the branch is not taken.
constexpr component<> folding = [] {
  auto &open = use_state(true);

  auto const toggle = [&open] { open = !*open; };

  if (!*open) {
    return button("closed") | on_click(toggle);
  }

  auto &clicks = use_state(0);

  auto const click = [&clicks] { clicks = *clicks + 1; };

  return div({button("close") | on_click(toggle), button("clicks " + std::to_string(*clicks)) | on_click(click)});
};

constexpr component<> folding_page = [] { return folding(); };

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

constexpr component<pair_props> pair_loader = [](pair_props const &given) {
  auto &sum = use_async_state<std::string>();
  auto &once = use_async_state<std::string>();

  use_effect(std::tuple{given.left, given.right}, load_sum, sum);
  use_effect(load_once, once);

  return p(sum->value + " " + once->value);
};

// What the test changes of pair_page.
struct pair_host {
  state<int> left{1};
  state<int> right{2};
};

constexpr component<> pair_page = [] {
  auto &host = use_context<pair_host>();

  return pair_loader({.left = *host->left, .right = *host->right});
};

// The function an effect runs is the one of the last view: what it captures is never stale.
std::vector<std::string> said;

struct greeter_props {
  int key = 0;
  std::string word;

  [[nodiscard]] bool operator==(greeter_props const &) const = default;
};

constexpr component<greeter_props> greeter = [](greeter_props const &given) {
  use_effect(given.key, [word = given.word](int key) -> task<> {
    said.push_back(word + " " + std::to_string(key));
    co_return;
  });

  return p(given.word);
};

// What the test changes of greeter_page.
struct greeter_host {
  state<int> key{1};
  state<std::string> word{"hello"};
};

constexpr component<> greeter_page = [] {
  auto &host = use_context<greeter_host>();

  return greeter({.key = *host->key, .word = *host->word});
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

constexpr component<> actions = [] {
  auto &store = use_context<counter_store>();
  auto &twice = use_action<&doubled>();
  auto &add = use_action<&counter_store::add>(store);

  return div({
      button("twice " + std::to_string(twice->value)) | on_click(twice.with(21)),
      button("total " + std::to_string(*store->total)) | on_click(add.with(5)),
  });
};

constexpr component<> actions_page = [] { return actions(); };

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

constexpr component<item_props> item_editor = [](item_props const &given) {
  auto &form = use_form(given.value);
  auto &save = use_action<&save_item>();

  return div({
      label({text("Stock"), input() | bind(form, &item::stock)}),
      when(!form.valid(), [] { return p("not a number"); }),
      button("Save") | disabled(!form.valid()) | on_click(save.with(form)),
  });
};

constexpr component<> item_page = [] { return item_editor({.value = {.name = "tea", .stock = 3}}); };

// A context provided to the components of a view, found before the app's.
struct theme {
  state<std::string> name{"light"};
};

constexpr component<> theme_shown = [] { return p(*use_context<theme>()->name); };

constexpr component<> dark_area = [] {
  auto &own = provide_context<theme>();

  // Set once: a state set at each view would ask for another without end.
  if (*own->name != "dark") {
    own->name.set("dark");
  }
  return div({theme_shown()});
};

constexpr component<> themes_page = [] { return div({theme_shown(), dark_area()}); };

// A component made of a struct: its props and helpers together, its state its hooks.
struct badge_view {
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

constexpr component<badge_view::props> badge = badge_view{};

// A struct declared as a class, with the component, as a header would, its operator() and its parts defined apart, and
// the component made of it, as a source would.
struct tally_view {
  struct props {
    std::string label;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element operator()(props const &given) const;

private:
  [[nodiscard]] static element count_of(state<int> &count);
};

extern component<tally_view::props> const tally;

element tally_view::operator()(props const &given) const {
  auto &count = use_state(0);

  return div({p(given.label), count_of(count)});
}

element tally_view::count_of(state<int> &count) {
  auto const add = [&count] { count = *count + 1; };

  return button(std::to_string(*count)) | on_click(add);
}

constexpr component<tally_view::props> tally = tally_view{};

constexpr component<> tally_page = [] { return tally({.label = "tally"}); };

// A component is no source of another: one with props given where one without is taken does not convert.
static_assert(!std::is_convertible_v<component<badge_view::props>, component<>>);
static_assert(!std::is_convertible_v<component<>, component<badge_view::props>>);

constexpr component<> badge_page = [] { return badge({.label = "badge"}); };

// A place whose hook changes type: refused.
constexpr component<> mismatched = [] {
  auto &flip = use_state(false);

  auto const toggle = [&flip] { flip = !*flip; };
  auto const here = std::source_location::current();

  if (*flip) {
    static_cast<void>(use_state(std::string{}, here));
  } else {
    static_cast<void>(use_state(0, here));
  }

  return button("flip") | on_click(toggle);
};

constexpr component<> mismatched_page = [] { return mismatched(); };

// A context nobody gave.
struct never_given {};

constexpr component<> missing_context = [] {
  static_cast<void>(use_context<never_given>());
  return p("never");
};

constexpr component<> missing_page = [] { return missing_context(); };

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

constexpr component<slow_props> slow_loader = [](slow_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_slow, text);
  return p("slow");
};

// What the test changes of going_page.
struct going_host {
  state<bool> shows{true};
};

constexpr component<> going_page = [] {
  auto &host = use_context<going_host>();

  return *host->shows ? slow_loader({.key = 1}) : p("gone");
};

constexpr component<loader_props> debounced_loader = [](loader_props const &given) {
  auto &text = use_async_state<std::string>();

  use_effect(given.key, load_text, text, {.debounce = 50ms});
  return p(text->value);
};

// What the test changes of debounced_page.
struct debounced_host {
  state<int> key{1};
};

constexpr component<> debounced_page = [] {
  auto &host = use_context<debounced_host>();

  return debounced_loader({.key = *host->key});
};

// A component made of a struct, of no props.
struct plain_badge_view {
  element operator()() const {
    auto &clicks = use_state(0);

    return p(std::to_string(*clicks));
  }
};

constexpr component<> plain_badge = plain_badge_view{};

constexpr component<> plain_badge_page = [] { return plain_badge(); };

// A function calling hooks called in a branch: the later calls of that place shift, as documented.
constexpr component<> conditional_counters = [] {
  auto &first_shown = use_state(true);

  auto const hide = [&first_shown] { first_shown = false; };

  return div({
      when(*first_shown, [] { return counted("a"); }),
      counted("b"),
      button("hide") | on_click(hide),
  });
};

constexpr component<> conditional_page = [] { return conditional_counters(); };

// An effect capturing `given` by reference, which its run keeps, read after the props changed.
std::vector<std::string> read_after;

struct named_props {
  int key = 0;
  std::string name;

  [[nodiscard]] bool operator==(named_props const &) const = default;
};

constexpr component<named_props> named_loader = [](named_props const &given) {
  use_effect(given.key, [&given](int) -> task<> {
    co_await quoll::async::sleep_for(10ms);
    read_after.push_back(given.name);
  });

  return p(given.name);
};

// What the test changes of named_page.
struct named_host {
  state<int> key{1};
  state<std::string> name{"a name long enough to be on the heap, not in a short string"};
};

constexpr component<> named_page = [] {
  auto &host = use_context<named_host>();

  return named_loader({.key = *host->key, .name = *host->name});
};

// A null function given to an effect.
constexpr component<> null_effect = [] {
  task<std::string> (*load)(int) = nullptr;
  auto &text = use_async_state<std::string>();

  use_effect(1, load, text);
  return p("never");
};

constexpr component<> null_effect_page = [] { return null_effect(); };

// A debounced effect capturing its props by reference, which writes what it read into its target.
task<std::string> name_after(std::string const &name) {
  co_await quoll::async::sleep_for(10ms);
  co_return name;
}

constexpr component<named_props> debounced_named = [](named_props const &given) {
  auto &read = use_async_state<std::string>();

  use_effect(given.key, [&given](int) { return name_after(given.name); }, read, {.debounce = 20ms});
  return p(read->value);
};

// What the test changes of debounced_named_page.
struct debounced_named_host {
  state<int> key{1};
  state<std::string> name{"the first name, long enough to be on the heap, not in a short string"};
};

constexpr component<> debounced_named_page = [] {
  auto &host = use_context<debounced_named_host>();

  return debounced_named({.key = *host->key, .name = *host->name});
};

// An action whose success is told to a closure capturing the props by reference, the props changed during the run.
std::vector<std::string> told_saved;

task<int> slow_double(int value) {
  co_await quoll::async::sleep_for(10ms);
  co_return value * 2;
}

constexpr component<named_props> saving_named = [](named_props const &given) {
  auto &save = use_action<&slow_double>([&given](int const &) { told_saved.push_back(given.name); });

  // A view that throws after writing what follows a success: it does not show, and writes nothing.
  if (given.key == 99) {
    throw std::runtime_error{"no view"};
  }

  return button(given.name) | on_click(save.with(given.key));
};

// What the test changes of saving_named_page.
struct saving_named_host {
  state<int> key{1};
  state<std::string> name{"the first name, long enough to be on the heap, not in a short string"};
};

constexpr component<> saving_named_page = [] {
  auto &host = use_context<saving_named_host>();

  return saving_named({.key = *host->key, .name = *host->name});
};

// A context it provides that uses one none gave, whose destructor reads it: refused before it is made.
struct lacking_context {
  shared<never_given> needed;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~lacking_context() { static_cast<void>(*needed); }
};

// A handler of one view that waits, then runs a save while later views show: the save tells the closure shown as it
// started, with that view's props, kept by the run.
constexpr component<named_props> waiting_saver = [](named_props const &given) {
  auto &save = use_action<&slow_double>([&given](int const &) { told_saved.push_back(given.name); });

  auto const later = [&save]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    static_cast<void>(co_await save.run(1));
  };

  return button(given.name) | on_click(later);
};

// What the test changes of waiting_saver_page.
struct waiting_saver_host {
  state<std::string> name{"the first name, long enough to be on the heap, not in a short string"};
};

constexpr component<> waiting_saver_page = [] {
  auto &host = use_context<waiting_saver_host>();

  return waiting_saver({.key = 1, .name = *host->name});
};

// An effect first made in a view that threw, then not called by the views that showed: it never runs.
std::vector<int> never_ran;

task<> note_run(int key) {
  never_ran.push_back(key);
  co_return;
}

constexpr component<> branching_effect = [] {
  auto &branch = use_state(false);
  auto &explode = use_state(false);

  auto const open = [&branch, &explode] {
    branch = true;
    explode = true;
  };

  auto const calm = [&branch, &explode] {
    branch = false;
    explode = false;
  };

  if (*branch) {
    use_effect(7, note_run);

    if (*explode) {
      throw std::runtime_error{"no view"};
    }
  }

  return div({button("open") | on_click(open), button("calm") | on_click(calm)});
};

constexpr component<> branching_page = [] { return branching_effect(); };

// An effect whose dependency a view that threw gave, then a view that showed without calling it: it does not run.
struct staged_props {
  int key = 0;
  bool calls = true;
  bool explodes = false;

  [[nodiscard]] bool operator==(staged_props const &) const = default;
};

constexpr component<staged_props> staged_effect = [](staged_props const &given) {
  if (given.calls) {
    use_effect(given.key, note_run);
  }

  if (given.explodes) {
    throw std::runtime_error{"no view"};
  }

  return p("staged");
};

// What the test changes of staged_page.
struct staged_host {
  state<staged_props> given{staged_props{.key = 1}};
};

constexpr component<> staged_page = [] {
  auto &host = use_context<staged_host>();

  return staged_effect(*host->given);
};

// A dependency whose assignment throws for 2, as a lack of memory copying it would.
struct touchy {
  int value = 0;

  explicit touchy(int given) : value(given) {}

  touchy(touchy const &) = default;
  touchy(touchy &&) = default;
  ~touchy() = default;

  touchy &operator=(touchy const &other) {
    if (other.value == 2) {
      throw std::runtime_error{"no touchy"};
    }

    value = other.value;
    return *this;
  }

  touchy &operator=(touchy &&other) { return *this = static_cast<touchy const &>(other); }

  [[nodiscard]] bool operator==(touchy const &) const = default;
};

std::vector<int> touched_with;

task<int> touch(touchy given) {
  touched_with.push_back(given.value);
  co_return given.value;
}

struct touchy_props {
  int key = 1;

  [[nodiscard]] bool operator==(touchy_props const &) const = default;
};

constexpr component<touchy_props> touchy_effect = [](touchy_props const &given) {
  auto &touched = use_async_state<int>();
  auto &run = use_effect(touchy{given.key}, touch, touched);

  auto const again = [&run] { run.reload(); };

  // Its target shows the failure, which asks for a view: one that would set the dependency again, without end.
  return button(touched->error ? std::string{"failed"} : std::to_string(touched->value)) | on_click(again);
};

// What the test changes of touchy_page.
struct touchy_host {
  state<int> key{1};
};

constexpr component<> touchy_page = [] {
  auto &host = use_context<touchy_host>();

  return touchy_effect({.key = *host->key});
};

// A state its own first view sets, as a struct's readying: the view after shows what it set.
constexpr component<> readying = [] {
  auto &ready = use_state(false);

  bool const was = *ready;

  if (!was) {
    ready = true;
  }

  return p(was ? "ready" : "not yet");
};

constexpr component<> readying_page = [] { return readying(); };

// A state made in a branch of a later view, and set by that view: the view after shows what it set.
constexpr component<> opening = [] {
  auto &open = use_state(false);

  auto const toggle = [&open] { open = true; };

  if (!*open) {
    return button("open") | on_click(toggle);
  }

  auto &done = use_state(false);

  bool const was = *done;

  if (!was) {
    done = true;
  }

  return p(was ? "done" : "not yet");
};

constexpr component<> opening_page = [] { return opening(); };

// A view that reads its effect's target before it calls use_effect: the view the first run's loading asks for shows it.
constexpr component<loader_props> reading_first = [](loader_props const &given) {
  auto &text = use_async_state<std::string>();

  bool const was_loading = text->loading;

  use_effect(given.key, load_text, text);
  return p(was_loading ? std::string{"loading"} : text->value);
};

constexpr component<> reading_first_page = [] { return reading_first({.key = 1}); };

} // namespace

TEST_CASE("function component: a state of its view, the same at each view", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, counter_page};

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
  test_app instance{time.get_executor(), shown, loader_page, {.contexts = {provide<loader_host>()}}};
  std::string first;
  std::string after_change;

  auto const act = [&]() -> task<> {
    first = shown.html();
    co_await quoll::async::sleep_for(20ms);
    instance.context<loader_host>().key = 2;
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
  test_app instance{time.get_executor(), shown, names_page, {.contexts = {provide<names>()}}};
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
  test_app instance{time.get_executor(), shown, two_counters_page};

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
  test_app instance{time.get_executor(), shown, folding_page};

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

TEST_CASE("function component: bind() of a state of its view binds an input to it", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, typing};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.children(shown.find("div")).at(0), "input", "tea");
    instance.dispatch(shown.children(shown.find("div")).at(1), "change", "true");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>tea ticked</p>"));
}

TEST_CASE("function component: a hook called outside the view of one is refused", "[ui][function]") {
  CHECK_THROWS_AS(use_state(1), std::logic_error);
}

TEST_CASE("function component: an effect on several values, and one on none that runs once", "[ui][function]") {
  ran.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, pair_page, {.contexts = {provide<pair_host>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.context<pair_host>().right = 5;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(ran == std::vector<std::string>{"sum 3", "once", "sum 6"});
  CHECK(shown.html() == "<p>6 once</p>");
}

TEST_CASE(
    "function component: the function an effect runs is the one of the view that asked for the run", "[ui][function]"
) {
  said.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, greeter_page, {.contexts = {provide<greeter_host>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<greeter_host>().word = "bye";
    co_await settle();
    instance.context<greeter_host>().key = 2;
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
  test_app instance{time.get_executor(), shown, actions_page, {.contexts = {provide<counter_store>()}}};

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
  test_app instance{time.get_executor(), shown, item_page};
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
  test_app instance{time.get_executor(), shown, themes_page, {.contexts = {provide<theme>()}}};

  auto const act = [&]() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><p>light</p><div><p>dark</p></div></div>");
}

TEST_CASE("function component: a struct whose operator() gives the view, its state its hooks", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, badge_page};

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
  test_app instance{time.get_executor(), shown, mismatched_page, {.on_error = collect(failures)}};

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
  test_app instance{time.get_executor(), shown, missing_page};

  CHECK_THROWS_AS(instance.start(), std::logic_error);
}

TEST_CASE("function component: an effect stops as its component goes", "[ui][function]") {
  lasting.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, going_page, {.contexts = {provide<going_host>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<going_host>().shows = false;
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(lasting == std::vector<std::string>{"start 1"});
}

TEST_CASE("function component: an effect's options, as a debounce, given to use_effect", "[ui][function]") {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, debounced_page, {.contexts = {provide<debounced_host>()}}};
  std::vector<std::string> waiting;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(20ms);
    instance.context<debounced_host>().key = 2;
    co_await quoll::async::sleep_for(20ms);
    instance.context<debounced_host>().key = 3;
    co_await quoll::async::sleep_for(40ms);
    waiting = loads;
    co_await quoll::async::sleep_for(30ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(waiting == std::vector<std::string>{"start 1"});
  CHECK(loads == std::vector<std::string>{"start 1", "start 3"});
}

TEST_CASE(
    "function component: a struct declared with its extern component, its parts defined apart, its state its hooks",
    "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, tally_page};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><p>tally</p><button>1</button></div>");
}

TEST_CASE("function component: a struct one of no props", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, plain_badge_page};

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
  test_app instance{time.get_executor(), shown, conditional_page};
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
  test_app instance{time.get_executor(), shown, named_page, {.contexts = {provide<named_host>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    // The props change while the run waits: it reads those of the view that started it.
    instance.context<named_host>().name = "another name long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(read_after == std::vector<std::string>{"a name long enough to be on the heap, not in a short string"});
}

TEST_CASE("function component: an effect given no function is refused", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, null_effect_page};

  CHECK_THROWS_AS(instance.start(), std::invalid_argument);
}

TEST_CASE(
    "function component: a debounced run calls the function of the view that asked for it, with its props",
    "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, debounced_named_page, {.contexts = {provide<debounced_named_host>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(20ms);
    // The view that asks for the debounced run: its function and its props are the run's.
    instance.context<debounced_named_host>().key = 2;
    instance.context<debounced_named_host>().name =
        "the second name, long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(5ms);
    // During the pause, then during the run: a view shown then writes a function no run asked of it calls.
    instance.context<debounced_named_host>().name =
        "the third name, long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(20ms);
    instance.context<debounced_named_host>().name =
        "the fourth name, long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>the second name, long enough to be on the heap, not in a short string</p>");
}

TEST_CASE(
    "function component: an action's success is told to the closure of the view it started from, a view "
    "that threw writing none",
    "[ui][function]"
) {
  told_saved.clear();

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app instance{
      time.get_executor(),
      shown,
      saving_named_page,
      {.on_error = collect(failures), .contexts = {provide<saving_named_host>()}}
  };

  auto const act = [&]() -> task<> {
    co_await settle();
    // A view that throws, with other props, then a click on the button of the view still shown: the save tells what
    // that view wrote, not what the one that threw did.
    instance.context<saving_named_host>().key = 99;
    instance.context<saving_named_host>().name =
        "the second name, long enough to be on the heap, not in a short string";
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // Each view after it throws, the one the save's end asks for too.
  REQUIRE_FALSE(failures.empty());
  CHECK(failures.front() == "no view");
  CHECK(told_saved == std::vector<std::string>{"the first name, long enough to be on the heap, not in a short string"});
}

namespace {

constexpr component<> lacking_area = [] {
  static_cast<void>(provide_context<lacking_context>());
  return p("never");
};

constexpr component<> lacking_page = [] { return lacking_area(); };

} // namespace

TEST_CASE(
    "function component: a context it provides that uses one not given is refused before it is made", "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, lacking_page};

  CHECK_THROWS_AS(instance.start(), std::logic_error);
}

TEST_CASE(
    "function component: an action started from an older view's handler tells the closure shown as it started",
    "[ui][function]"
) {
  told_saved.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, waiting_saver_page, {.contexts = {provide<waiting_saver_host>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    // Before the save starts: the closure shown then is this view's.
    instance.context<waiting_saver_host>().name =
        "the second name, long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(7ms);
    // During the save: its closure and its props stay.
    instance.context<waiting_saver_host>().name =
        "the third name, long enough to be on the heap, not in a short string";
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(
      told_saved == std::vector<std::string>{"the second name, long enough to be on the heap, not in a short string"}
  );
}

TEST_CASE(
    "function component: an effect made in a view that threw, and called by none that showed, never runs",
    "[ui][function]"
) {
  never_ran.clear();

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app instance{time.get_executor(), shown, branching_page, {.on_error = collect(failures)}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find_text("open"), "click");
    co_await settle();
    instance.dispatch(shown.find_text("calm"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(failures.empty());
  CHECK(never_ran.empty());
}

TEST_CASE(
    "function component: a dependency given by a view that threw is not one an effect runs with", "[ui][function]"
) {
  never_ran.clear();

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app instance{
      time.get_executor(), shown, staged_page, {.on_error = collect(failures), .contexts = {provide<staged_host>()}}
  };

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<staged_host>().given = staged_props{.key = 2, .explodes = true};
    co_await settle();
    instance.context<staged_host>().given = staged_props{.key = 2, .calls = false};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failures == std::vector<std::string>{"no view"});
  CHECK(never_ran == std::vector<int>{1});
}

TEST_CASE(
    "function component: a dependency that throws as it is set fails the effect once, as a struct's", "[ui][function]"
) {
  touched_with.clear();

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app instance{
      time.get_executor(), shown, touchy_page, {.on_error = collect(failures), .contexts = {provide<touchy_host>()}}
  };

  std::string failed_shown;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<touchy_host>().key = 2;
    co_await settle();
    co_await settle();
    failed_shown = shown.html();
    // Another value, then reload(): it runs again, with what the view that showed gave.
    instance.context<touchy_host>().key = 3;
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed_shown == "<button>failed</button>");
  CHECK(touched_with == std::vector<int>{1, 3});
  CHECK(failures == std::vector<std::string>{"no touchy"});
}

TEST_CASE("function component: a state its view sets, the view that made it included, shows again", "[ui][function]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, readying_page};

  auto const act = [&]() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>ready</p>");
}

TEST_CASE(
    "function component: a state made in a branch of a later view, and set by it, shows again", "[ui][function]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, opening_page};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>done</p>");
}

TEST_CASE(
    "function component: a view reading its target before use_effect shows it loading the view after", "[ui][function]"
) {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, reading_first_page};
  std::string meanwhile;

  auto const act = [&]() -> task<> {
    co_await settle();
    meanwhile = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(meanwhile == "<p>loading</p>");
  CHECK(shown.html() == "<p>text 1</p>");
}
