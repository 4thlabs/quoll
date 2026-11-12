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
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/provided.hpp>
#include <quoll/ui/shared.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
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

// What the test running saw happen.
std::vector<std::string> lines;

task<std::vector<std::string>> load_names() {
  co_await quoll::async::sleep_for(1ms);
  lines.emplace_back("loaded");
  co_return std::vector<std::string>{"tea", "coffee"};
}

// Data the components of an app share: names, loaded as it first shows, and renamed through run().
struct names {
  state<std::tuple<std::vector<std::string>, bool, std::exception_ptr>> all;
  effect load{std::tie(), load_names, all};

  task<std::string> rename(int index, std::string name) {
    lines.push_back("rename " + name);
    co_await quoll::async::sleep_for(10ms);

    if (name.empty()) {
      throw std::runtime_error{"no name"};
    }

    all.update([&](auto &held) { std::get<0>(held).at(static_cast<std::size_t>(index)) = name; });
    lines.push_back("renamed " + name);
    co_return name;
  }
};

// Shows the names, through its context: no props, so that its view runs again only as what it reads changes.
int list_views = 0;

struct name_list {
  shared<names> shown;

  element view() {
    ++list_views;

    auto const &[all, loading, error] = *shown->all;
    std::string text = loading ? "loading" : "";

    for (auto const &name : all) {
      text += name + ";";
    }

    return p(text);
  }
};

// Renames the first name as it first shows, through its context, and may go before the rename ends.
struct renamer {
  shared<names> data;
  state<std::string> gave;

  task<> mounted() {
    auto const given = co_await data.run(&names::rename, 0, std::string{"green tea"});

    gave = given;
  }

  element view() { return p("renamer " + *gave); }
};

struct names_page {
  state<bool> renames;

  element view() {
    auto const editor = [] { return component<renamer>(); };

    return div({component<name_list>(), when(*renames, editor)});
  }
};

// Uses a context its app was not given.
struct orphan_page {
  shared<names> missing;

  element view() { return p("orphan"); }
};

struct orphan_child_page {
  element view() { return div({component<name_list>()}); }
};

// A context using another.
struct greeting {
  shared<names> from;

  [[nodiscard]] std::string first() const {
    auto const &all = std::get<0>(*from->all);

    return all.empty() ? "nobody" : "hello " + all.front();
  }
};

struct greeting_page {
  shared<greeting> said;

  element view() { return p(said->first()); }
};

// Reads the names as it goes: given after them, it goes before them.
// An aggregate, as a context is, which declares no constructor: its shared<names> is neither copied nor moved already.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
struct last_word {
  shared<names> from;

  ~last_word() { lines.push_back("last word " + std::to_string(std::get<0>(*from->all).size())); }
};

// Say as they go, in that order: the second, using the first, goes before it.
struct first_gone {
  state<int> count;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~first_gone() { lines.emplace_back("first gone"); }
};

// Writes to the first as it goes: its app, going too, hears nothing of it.
struct second_gone {
  shared<first_gone> from;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~second_gone() {
    from->count = 1;
    lines.emplace_back("second gone");
  }
};

// Uses the first, then a context no app is given: its making fails past the first, and it writes to it as it goes.
struct context_never_given {};

struct broken_user {
  shared<first_gone> from;
  shared<context_never_given> missing;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~broken_user() {
    from->count = 1;
    lines.emplace_back("broken gone");
  }
};

// An executor that counts what is posted to it.
struct counting_executor {
  virtual_time::executor inner;
  int *posts;

  void post(quoll::async::work_item &item) const {
    ++*posts;
    inner.post(item);
  }

  [[nodiscard]] bool operator==(counting_executor const &) const noexcept = default;
};

// Writes to the first as it goes, and uses a context no app is given: refused before it is made.
struct broken_component {
  shared<first_gone> from;
  shared<context_never_given> missing;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~broken_component() {
    from->count = 1;
    lines.emplace_back("component gone");
  }

  element view() { return p("broken"); }
};

struct broken_child_page {
  element view() { return div({component<broken_component>()}); }
};

// Writes to its context as it goes, as its app goes too.
struct leaving_page {
  shared<first_gone> from;

  // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions): an aggregate.
  ~leaving_page() { from->count = 2; }

  element view() { return p("leaving"); }
};

// Reads its count, and sets it in its view the first time: the view the change asks for shows the count set.
int counting_views = 0;

struct counting_child {
  shared<first_gone> data;

  element view() {
    ++counting_views;

    auto const seen = *data->count;

    if (seen == 0) {
      data->count = 1;
    }

    return p("count " + std::to_string(seen));
  }
};

// Reads its count, and throws while asked to: a view that failed is run again by the next render.
bool child_throws = false;

struct throwing_child {
  shared<first_gone> data;

  element view() {
    auto const seen = *data->count;

    if (child_throws) {
      throw std::runtime_error{"no view"};
    }

    return p("count " + std::to_string(seen));
  }
};

struct throwing_child_page {
  state<int> other;

  element view() { return div({p(std::to_string(*other)), component<throwing_child>()}); }
};

struct counting_child_page {
  element view() { return div({component<counting_child>()}); }
};

// Shows the names once asked to, after they changed.
struct later_list_page {
  state<bool> shows;

  element view() {
    auto const names_shown = [] { return component<name_list>(); };

    return div({when(*shows, names_shown)});
  }
};

struct counting_page {
  shared<second_gone> said;

  element view() { return p("counting"); }
};

// A component that only runs on its context, and counts its views.
int runner_views = 0;

struct name_runner {
  shared<names> data;

  element view() {
    ++runner_views;

    auto const rename = [this]() -> task<> {
      static_cast<void>(co_await data.run(&names::rename, 1, std::string{"mate"}));
    };

    return button("rename") | on_click(rename);
  }
};

struct runner_page {
  element view() { return div({component<name_runner>(), component<name_list>()}); }
};

// The names, as its own member: run() from outside, once the app stopped.
struct holding_page {
  shared<names> data;

  element view() { return p("holding"); }
};

// The greeting in a component of a view: no props, so that its view runs again only as what it reads changes.
struct greeting_line {
  shared<greeting> said;

  element view() { return p(said->first()); }
};

struct greeting_child_page {
  element view() { return div({component<greeting_line>()}); }
};

// A draft of names, which a panel provides to the components of its view.
struct draft {
  state<std::vector<std::string>> lines;
  state<std::tuple<std::vector<std::string>, bool, std::exception_ptr>> loaded;
  effect load{std::tie(), load_names, loaded};
};

// Shows the draft it finds: the nearest provided above, or the app's.
struct draft_view {
  shared<draft> shown;

  element view() {
    std::string text;

    for (auto const &line : *shown->lines) {
      text += line + ";";
    }

    for (auto const &name : std::get<0>(*shown->loaded)) {
      text += "loaded " + name + ";";
    }

    return p("draft " + text);
  }
};

// Adds to the draft it finds, on a click.
struct draft_adder {
  shared<draft> added;

  element view() {
    auto const add = [this] { added->lines.update([](auto &all) { all.emplace_back("typed"); }); };

    return button("add") | on_click(add);
  }
};

struct draft_panel {
  provided<draft> mine;

  element view() { return div({component<draft_adder>(), component<draft_view>()}); }
};

// A panel providing its draft, and beside it a view of the app's.
struct drafts_page {
  state<bool> shows{true};

  element view() {
    auto const panel = [] { return component<draft_panel>(); };

    return div({when(*shows, panel), component<draft_view>()});
  }
};

// The app's own component provides a draft to the components of its view.
struct providing_root {
  provided<draft> mine;

  element view() { return div({component<draft_adder>(), component<draft_view>()}); }
};

// A context of the app using a draft, as the app's own component provides one too.
struct draft_user {
  shared<draft> used;
};

// Greets through what it provides, which uses the app's names: a change of them shows it again.
struct local_greeting {
  shared<names> from;
};

struct greeting_holder {
  provided<local_greeting> mine;

  element view() {
    auto const &all = std::get<0>(*mine->from->all);

    return p(all.empty() ? "holder nobody" : "holder " + all.front());
  }
};

struct greeting_holder_page {
  element view() { return div({component<greeting_holder>()}); }
};

// The app's own component uses the app's draft, and provides one of its own to the components of its view.
struct using_and_providing_root {
  shared<draft> app_draft;
  provided<draft> mine;

  element view() { return div({component<draft_view>()}); }
};

} // namespace

TEST_CASE("context: its effects start after the app's first view, and its components show what it holds",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<names_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};
  std::string loading;

  auto const act = [&]() -> task<> {
    co_await settle();
    loading = shown.html();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loading.contains("<p>loading</p>"));
  CHECK(shown.html().contains("<p>tea;coffee;</p>"));
  CHECK(lines == std::vector<std::string>{"loaded"});
}

TEST_CASE("context: a change of its states shows again a component using it, whose own states did not change",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<names_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.context<names>().all.update([](auto &held) { std::get<0>(held).emplace_back("mate"); });
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>tea;coffee;mate;</p>"));
}

TEST_CASE("context: run() gives what the context's work gave, and the work goes on as its caller goes",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<names_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};
  std::string given;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);

    // Shown, it renames; its caller waits, and shows what it gave.
    instance.component().renames = true;
    co_await quoll::async::sleep_for(20ms);
    given = shown.html();

    // Shown again, then gone before its rename ends: the rename goes on, and the list shows it.
    instance.component().renames = false;
    co_await settle();
    instance.component().renames = true;
    co_await settle();
    instance.context<names>().all.update([](auto &held) { std::get<0>(held).at(0) = "tea"; });
    co_await settle();
    instance.component().renames = false;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(given.contains("<p>renamer green tea</p>"));
  CHECK(lines == std::vector<std::string>{"loaded", "rename green tea", "renamed green tea", "rename green tea",
                                          "renamed green tea"});
  CHECK(shown.html().contains("<p>green tea;coffee;</p>"));
  CHECK_FALSE(shown.html().contains("renamer"));
}

TEST_CASE("context: what its work throws once its caller went goes to on_error", "[ui][context]") {
  lines.clear();

  struct failing_renamer {
    shared<names> data;

    task<> mounted() { static_cast<void>(co_await data.run(&names::rename, 0, std::string{})); }

    element view() { return p("renamer"); }
  };

  struct failing_page {
    state<bool> renames{true};

    element view() {
      auto const editor = [] { return component<failing_renamer>(); };

      return div({when(*renames, editor)});
    }
  };

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app<failing_page> instance{time.get_executor(),
                                  shown,
                                  {.on_error =
                                       [&failures](std::exception_ptr failure) {
                                         try {
                                           std::rethrow_exception(failure);
                                         } catch (std::exception const &error) {
                                           failures.emplace_back(error.what());
                                         }
                                       },
                                   .contexts = {provide<names>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().renames = false;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failures == std::vector<std::string>{"no name"});
}

TEST_CASE("context: one its app was not given is refused as the component using it is made", "[ui][context]") {
  virtual_time time;
  memory_renderer const shown;

  CHECK_THROWS_AS((test_app<orphan_page>{time.get_executor(), shown}), std::logic_error);

  test_app<orphan_child_page> instance{time.get_executor(), shown};
  auto const start = [&]() -> task<int> {
    bool refused = false;

    try {
      instance.start();
    } catch (std::logic_error const &) {
      refused = true;
    }

    co_await instance.stop();
    co_return refused ? 0 : 1;
  };

  CHECK(quoll::test::run(time, start()) == 0);
  CHECK_THROWS_AS(instance.context<names>(), std::logic_error);
}

TEST_CASE("context: two of one type are refused", "[ui][context]") {
  virtual_time time;
  memory_renderer const shown;

  CHECK_THROWS_AS(
      (test_app<names_page>{time.get_executor(), shown, {.contexts = {provide<names>(), provide<names>()}}}),
      std::logic_error);
}

TEST_CASE("context: one may use another given before it, whose changes show where it is read", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<greeting_child_page> instance{
      time.get_executor(), shown, {.contexts = {provide<names>(), provide<greeting>()}}};
  std::string before;

  auto const act = [&]() -> task<> {
    co_await settle();
    before = shown.html();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before.contains("<p>nobody</p>"));
  CHECK(shown.html().contains("<p>hello tea</p>"));
}

TEST_CASE("context: one using another given after it is refused, so that none uses one that uses it", "[ui][context]") {
  virtual_time time;
  memory_renderer const shown;

  CHECK_THROWS_AS(
      (test_app<greeting_page>{time.get_executor(), shown, {.contexts = {provide<greeting>(), provide<names>()}}}),
      std::logic_error);
}

TEST_CASE("context: a component a view written once shows, with no app, cannot use one", "[ui][context]") {
  CHECK_THROWS_AS(to_html(div({component<name_list>()})), std::logic_error);
}

TEST_CASE("context: one using another goes before it, which it may still read", "[ui][context]") {
  lines.clear();

  {
    virtual_time time;
    memory_renderer const shown;
    test_app<greeting_page> instance{
        time.get_executor(), shown, {.contexts = {provide<names>(), provide<greeting>(), provide<last_word>()}}};

    auto const act = [&]() -> task<> { co_await quoll::async::sleep_for(5ms); };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  }

  CHECK(lines == std::vector<std::string>{"loaded", "last word 2"});
}

TEST_CASE("context: the contexts go the last given first, telling their app of nothing as they go", "[ui][context]") {
  lines.clear();

  {
    virtual_time time;
    memory_renderer const shown;

    // Never started: what the second writes to the first as it goes reaches no app.
    test_app<counting_page> instance{
        time.get_executor(), shown, {.contexts = {provide<first_gone>(), provide<second_gone>()}}};
  }

  CHECK(lines == std::vector<std::string>{"second gone", "first gone"});
}

TEST_CASE("context: a component that only runs on its context does not show again as the context changes",
          "[ui][context]") {
  lines.clear();
  runner_views = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app<runner_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};
  int before = 0;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    before = runner_views;

    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before == 1);
  CHECK(runner_views == 1);
  CHECK(shown.html().contains("<p>tea;mate;</p>"));
}

TEST_CASE("context: run() once its app stopped runs nothing, and ends its caller on a stop", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<holding_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};

  auto const act = [&]() -> task<int> {
    instance.start();
    co_await instance.stop();

    auto const given = co_await quoll::async::stopped_as_optional(
        instance.component().data.run(&names::rename, 0, std::string{"late"}));

    co_return given ? 1 : 0;
  };

  REQUIRE(quoll::test::run(time, act()) == 0);
  CHECK(std::ranges::find(lines, "rename late") == lines.end());
  CHECK(std::get<0>(*std::as_const(instance).context<names>().all).empty());
}

TEST_CASE("context: one that cannot find those it uses fails the app's making, never made, and tells it of nothing",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  int posts = 0;

  CHECK_THROWS_AS((test_app<counting_page>{counting_executor{time.get_executor(), &posts},
                                           shown,
                                           {.contexts = {provide<first_gone>(), provide<broken_user>()}}}),
                  std::logic_error);

  // The broken one never made, the app, half made, was asked for no render.
  CHECK(posts == 0);
  CHECK(lines == std::vector<std::string>{"first gone"});
}

TEST_CASE("context: the app's own component missing one fails its making, never made, and tells it of nothing",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  int posts = 0;

  // Its context not found: it is never made, and the app, half made, hears of nothing.
  CHECK_THROWS_AS((test_app<broken_component>{
                      counting_executor{time.get_executor(), &posts}, shown, {.contexts = {provide<first_gone>()}}}),
                  std::logic_error);
  CHECK(posts == 0);
  CHECK(lines == std::vector<std::string>{"first gone"});
}

TEST_CASE("context: a component of a view missing one is refused before it is made", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<broken_child_page> instance{time.get_executor(), shown, {.contexts = {provide<first_gone>()}}};

  auto const act = [&]() -> task<int> {
    bool refused = false;

    try {
      instance.start();
    } catch (std::logic_error const &) {
      refused = true;
    }

    co_await instance.stop();
    co_return refused ? 0 : 1;
  };

  REQUIRE(quoll::test::run(time, act()) == 0);
  CHECK(lines.empty());
}

TEST_CASE("context: what a component writes to one as its app goes, never started, asks for no render",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  int posts = 0;

  {
    test_app<leaving_page> instance{
        counting_executor{time.get_executor(), &posts}, shown, {.contexts = {provide<first_gone>()}}};
  }

  CHECK(posts == 0);
}

TEST_CASE("context: a component shown after its context changed shows once", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<later_list_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    list_views = 0;
    instance.component().shows = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>tea;coffee;</p>"));
  CHECK(list_views == 1);
}

TEST_CASE("context: a view that changes the context it reads shows again", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<counting_child_page> instance{time.get_executor(), shown, {.contexts = {provide<first_gone>()}}};

  auto const act = [&]() -> task<> { co_await settle(); };

  counting_views = 0;

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>count 1</p>"));
  CHECK(counting_views == 2);
}

TEST_CASE("context: a component whose view threw after its context changed views again in the next render",
          "[ui][context]") {
  lines.clear();
  child_throws = false;

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app<throwing_child_page> instance{time.get_executor(),
                                         shown,
                                         {.on_error =
                                              [&failures](std::exception_ptr failure) {
                                                try {
                                                  std::rethrow_exception(failure);
                                                } catch (std::exception const &error) {
                                                  failures.emplace_back(error.what());
                                                }
                                              },
                                          .contexts = {provide<first_gone>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();

    // Its context changes, and its view throws: that render fails.
    child_throws = true;
    instance.context<first_gone>().count = 5;
    co_await settle();
    child_throws = false;

    // The next render, for a state of the page, views it again, with what its context holds now.
    instance.component().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failures == std::vector<std::string>{"no view"});
  CHECK(shown.html().contains("<p>count 5</p>"));
}

TEST_CASE("context: the app's own component, or a context, missing one is never made, as its destructor may read it",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;

  // Their destructors read what they would have found: a throw there would end the program.
  CHECK_THROWS_AS((test_app<broken_component>{time.get_executor(), shown}), std::logic_error);
  CHECK_THROWS_AS((test_app<counting_page>{time.get_executor(), shown, {.contexts = {provide<broken_user>()}}}),
                  std::logic_error);
  CHECK(lines.empty());
}

TEST_CASE("context: one a component provides is found by those of its view before the app's, and shows their changes",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<drafts_page> instance{time.get_executor(), shown, {.contexts = {provide<draft>()}}};
  std::string added;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);

    // The panel's adder adds to the panel's draft: its view shows it, the view of the app's does not.
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    added = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  // Both loaded, the panel's by its own effect, and the app's.
  CHECK(added.contains("<p>draft typed;loaded tea;loaded coffee;</p>"));
  CHECK(added.contains("<p>draft loaded tea;loaded coffee;</p>"));
}

TEST_CASE("context: one a component provides goes with it", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<drafts_page> instance{time.get_executor(), shown, {.contexts = {provide<draft>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The panel goes, and comes back: its draft is a new one.
    instance.component().shows = false;
    co_await settle();
    instance.component().shows = true;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK_FALSE(shown.html().contains("typed"));
}

TEST_CASE("context: one the app's own component provides is found by the components of its view", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  // No context of the app: what the components find is the app's own component's.
  test_app<providing_root> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>draft typed;loaded tea;loaded coffee;</p>"));
}

TEST_CASE("context: the app's contexts are made before its own component, which they do not find", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  // The app's own component provides a draft too, made after the app's contexts: the user finds the app's.
  test_app<providing_root> instance{
      time.get_executor(), shown, {.contexts = {provide<draft>(), provide<draft_user>()}}};

  auto const act = [&]() -> task<> { co_await quoll::async::sleep_for(5ms); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(&*instance.context<draft_user>().used == &instance.context<draft>());
}

TEST_CASE("context: a component shows again as a context what it provides uses changes", "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<greeting_holder_page> instance{time.get_executor(), shown, {.contexts = {provide<names>()}}};
  std::string before;

  auto const act = [&]() -> task<> {
    co_await settle();
    before = shown.html();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before.contains("<p>holder nobody</p>"));
  CHECK(shown.html().contains("<p>holder tea</p>"));
}

TEST_CASE("context: the app's own component finds the app's contexts, what it provides being for those of its view",
          "[ui][context]") {
  lines.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<using_and_providing_root> instance{time.get_executor(), shown, {.contexts = {provide<draft>()}}};

  auto const act = [&]() -> task<> { co_await quoll::async::sleep_for(5ms); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(&*instance.component().app_draft == &instance.context<draft>());
  CHECK(&*instance.component().app_draft != &*instance.component().mine);
}
