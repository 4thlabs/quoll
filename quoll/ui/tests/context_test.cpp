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
#include <quoll/ui/shared.hpp>

#include <catch2/catch_test_macros.hpp>

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
struct name_list {
  shared<names> shown;

  element view() {
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

// The greeting in a component of a view: no props, so that its view runs again only as what it reads changes.
struct greeting_line {
  shared<greeting> said;

  element view() { return p(said->first()); }
};

struct greeting_child_page {
  element view() { return div({component<greeting_line>()}); }
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
