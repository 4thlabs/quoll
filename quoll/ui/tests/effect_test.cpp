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
#include <quoll/async/when_all.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/detail/effect_starter.hpp>
#include <quoll/ui/effect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
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

// The runs of the test running: how long each key takes, which fail or ignore their stop, and what happened.
struct load_log {
  std::map<int, std::chrono::milliseconds> delays;
  std::set<int> failing;
  // Keys whose run ignores its stop: it yields to the executor rather than wait on a timer, its resumption queued
  // before the stop is asked for, and ends with no other wait, as an answer already on its way does.
  std::set<int> stubborn;
  std::vector<std::string> lines;
  int stopped = 0;
};

load_log loads;

task<std::string> load_text(int key) {
  loads.lines.push_back("start " + std::to_string(key));

  // Destroyed without having ended: the run was stopped, and its frame destroyed from the top.
  struct watch {
    bool ended = false;

    watch() = default;
    watch(watch const &) = delete;
    watch(watch &&) = delete;
    watch &operator=(watch const &) = delete;
    watch &operator=(watch &&) = delete;

    ~watch() {
      if (!ended) {
        ++loads.stopped;
      }
    }
  } watched;

  if (loads.stubborn.contains(key)) {
    for (int turn = 0; turn < 3; ++turn) {
      co_await quoll::async::yield();
    }
  } else {
    auto const delay = loads.delays.find(key);

    co_await quoll::async::sleep_for(delay == loads.delays.end() ? 1ms : delay->second);
  }

  if (loads.failing.contains(key)) {
    throw std::runtime_error{"no text " + std::to_string(key)};
  }

  watched.ended = true;
  loads.lines.push_back("end " + std::to_string(key));
  co_return "text " + std::to_string(key);
}

// Throws for a key below zero before giving a task, as a function checking its argument first does.
task<std::string> checked_text(int key) {
  if (key < 0) {
    throw std::invalid_argument{"no such key"};
  }

  return load_text(key);
}

// What a page loads: the text, whether it is loading, and why it did not load.
using text_target = std::tuple<std::string, bool, std::exception_ptr>;

// What a target shows, in words: the values before stay while a run is under way.
std::string shown_of(text_target const &text) {
  auto const &[value, loading, error] = text;

  if (loading) {
    return "running";
  }

  if (error) {
    return "failed";
  }

  return value.empty() ? "nothing" : value;
}

// Loads the text of the key it holds, as it changes; a click loads it again.
struct loading_page {
  state<int> next{1};
  state<text_target> text;
  effect load{next, load_text, text};

  element view() {
    auto const again = [this] { load.reload(); };

    return div({button("again") | on_click(again), p(shown_of(*text))});
  }
};

// Loads with a loader that may throw before its task.
struct checked_page {
  state<int> next{1};
  state<text_target> text;
  effect load{next, checked_text, text};

  element view() { return p(shown_of(*text)); }
};

// Loads only once a key is chosen.
struct choosing_page {
  state<std::optional<int>> chosen;
  state<text_target> text;
  effect load{chosen, load_text, text};

  element view() { return p(shown_of(*text)); }
};

// Its target holds the value alone: a failure goes to the app's on_error.
struct plain_page {
  state<int> next{1};
  state<std::string> text;
  effect load{next, load_text, text};

  element view() { return p(*text); }
};

// Two loads, of `left` and of `left` + 100, the second given `right` too, written at once.
task<std::tuple<std::string, std::string>> load_both(int left, std::string right) {
  auto [first, second] = co_await quoll::async::when_all(load_text(left), load_text(left + 100));

  co_return {std::move(first), std::move(second) + " " + right};
}

struct both_page {
  state<int> left{1};
  state<std::string> right{"a"};
  state<std::tuple<std::string, std::string, bool, std::exception_ptr>> both;
  effect load{std::tie(left, right), load_both, both};

  element view() {
    auto const &[first, second, loading, error] = *both;

    return p(first + "|" + second + (loading ? " running" : "") + (error ? " failed" : ""));
  }
};

// Loads once, as its first view shows: it depends on nothing.
struct once_page {
  state<int> other;
  state<text_target> text;
  effect load{std::tie(), [] { return load_text(5); }, text};

  element view() { return p(shown_of(*text) + " " + std::to_string(*other)); }
};

// A panel loading as it shows, which its page may take away.
struct loading_panel {
  state<int> key{7};
  state<text_target> text;
  effect load{key, load_text, text};

  element view() { return p(shown_of(*text)); }
};

struct panel_page {
  state<bool> shows{true};

  element view() {
    auto const panel = [] { return component<loading_panel>(); };

    return div({when(*shows, panel)});
  }
};

// Loads as `next` changes; `other` changes nothing it depends on, but views it again.
struct other_page {
  state<int> next{1};
  state<int> other;
  state<text_target> text;
  effect load{next, load_text, text};

  element view() { return p(shown_of(*text) + " " + std::to_string(*other)); }
};

// An executor that can be made to fail.
struct failing_executor {
  virtual_time::executor inner;
  bool *fail;

  void post(quoll::async::work_item &item) const {
    if (*fail) {
      throw std::runtime_error{"no more"};
    }

    inner.post(item);
  }

  [[nodiscard]] bool operator==(failing_executor const &) const noexcept = default;
};

// What a failure says.
std::string what_of(std::exception_ptr const &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (std::exception const &error) {
    return error.what();
  }
}

// Searches for what is typed, a while after the last change: a debounce.
std::vector<std::string> searched;

struct searching_page {
  state<std::string> query;

  effect search{query, [](std::string text) -> task<> {
                  co_await quoll::async::sleep_for(300ms);
                  searched.push_back(std::move(text));
                }};

  element view() { return input() | bind(query); }
};

// Keeps what an effect starts, as its app would after a view, for the test to run when it chooses.
struct kept_starts {
  std::vector<task<>> started;

  [[nodiscard]] quoll::ui::detail::effect_starter starter() {
    return {.owner = &started, .start = [](void *owner, task<> work) {
              static_cast<std::vector<task<>> *>(owner)->push_back(std::move(work));
            }};
  }

  // Runs what was started since the last call.
  task<int> run_all() {
    auto taken = std::exchange(started, {});

    for (auto &work : taken) {
      co_await std::move(work);
    }

    co_return 0;
  }
};

// An effect out of any app, its views made by the test.
struct bare_page {
  state<std::optional<int>> chosen{std::optional<int>{1}};
  state<text_target> text;
  effect load{chosen, load_text, text};
};

// A key whose copy, or comparison, throws once asked to, as one that allocates may.
bool key_copy_throws = false;
bool key_compare_throws = false;

// Asks a key to throw for as long as it lives, whatever the test does meanwhile.
struct throwing_keys {
  bool &flag;

  explicit throwing_keys(bool &asked) : flag(asked) { flag = true; }

  throwing_keys(throwing_keys const &) = delete;
  throwing_keys(throwing_keys &&) = delete;
  throwing_keys &operator=(throwing_keys const &) = delete;
  throwing_keys &operator=(throwing_keys &&) = delete;

  ~throwing_keys() { flag = false; }
};

struct touchy_key {
  int value = 0;

  touchy_key() = default;

  explicit touchy_key(int given) : value(given) {}

  touchy_key(touchy_key const &other) : value(other.value) {
    if (key_copy_throws) {
      throw std::runtime_error{"no copy"};
    }
  }

  touchy_key(touchy_key &&) noexcept = default;
  touchy_key &operator=(touchy_key const &) = default;
  touchy_key &operator=(touchy_key &&) noexcept = default;
  ~touchy_key() = default;

  [[nodiscard]] bool operator==(touchy_key const &other) const {
    if (key_compare_throws) {
      throw std::runtime_error{"no compare"};
    }

    return value == other.value;
  }
};

task<std::string> load_key(touchy_key given) { return load_text(given.value); }

struct touchy_key_page {
  state<touchy_key> key{touchy_key{1}};
  state<text_target> text;
  effect load{key, load_key, text};
};

// Its target has no place for an error: a failure to start is thrown.
struct touchy_key_loading_page {
  state<touchy_key> key{touchy_key{1}};
  state<std::tuple<std::string, bool>> text;
  effect load{key, load_key, text};
};

} // namespace

TEST_CASE("effect: runs once its component first shows, its target loading, then holding what it gave",
          "[ui][effect]") {
  loads = {};
  loads.delays = {{1, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string running;

  auto const act = [&]() -> task<> {
    co_await settle();
    running = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(running.contains("<p>running</p>"));
  CHECK(shown.html().contains("<p>text 1</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1"});
}

TEST_CASE("effect: a change of its state stops the run before, whose answer writes nothing", "[ui][effect]") {
  // The first run answers after the second.
  loads = {};
  loads.delays = {{1, 10ms}, {2, 1ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().next = 2;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>text 2</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "start 2", "end 2"});
  CHECK(loads.stopped == 1);
}

TEST_CASE("effect: a run stopped by a later one that ends anyway writes nothing", "[ui][effect]") {
  // The first run ignores its stop, and ends after the second has begun.
  loads = {};
  loads.stubborn = {1};
  loads.delays = {{2, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string meanwhile;

  auto const act = [&]() -> task<> {
    // The first run waits on its yields; the change, rendered at once, starts the second during them.
    co_await quoll::async::yield();
    instance.component().next = 2;
    co_await settle();
    meanwhile = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(loads.lines == std::vector<std::string>{"start 1", "start 2", "end 1", "end 2"});
  CHECK(meanwhile.contains("<p>running</p>"));
  CHECK(shown.html().contains("<p>text 2</p>"));
}

TEST_CASE("effect: an answer of the run before, come after the view of a change, writes nothing", "[ui][effect]") {
  // The first run ignores its stop. The view of the change comes after some turns of its yields, before it ends, and
  // the second run starts only after it has ended: what it gave comes after that view, which stopped it.
  for (int before = 0; before < 3; ++before) {
    loads = {};
    loads.stubborn = {1};

    bare_page page;
    kept_starts starts;
    std::vector<std::string> seen;

    page.load.after_view(starts.starter());

    auto const change = [&]() -> task<> {
      for (int turn = 0; turn < before; ++turn) {
        co_await quoll::async::yield();
      }

      page.chosen = std::optional<int>{2};
      page.load.after_view(starts.starter());

      for (int turn = 0; turn < 6; ++turn) {
        co_await quoll::async::yield();
        seen.push_back(std::get<0>(*page.text));
      }

      co_await std::move(starts.started.back());
    };

    auto const act = [&]() -> task<int> {
      co_await quoll::async::when_all(std::move(starts.started.front()), change());
      co_return 0;
    };

    virtual_time time;

    REQUIRE(quoll::test::run(time, act()) == 0);
    REQUIRE(loads.lines == std::vector<std::string>{"start 1", "end 1", "start 2", "end 2"});
    CHECK(std::ranges::find(seen, "text 1") == seen.end());
    CHECK(std::get<0>(*page.text) == "text 2");
  }
}

TEST_CASE("effect: a run asked for after a view writes nothing once a later view asked for another", "[ui][effect]") {
  // Two views before the executor runs what the first started, as two synchronous starts make: the first run, queued,
  // was replaced as it was asked for, and runs nothing.
  loads = {};

  SECTION("a value") {
    bare_page page;
    kept_starts starts;

    page.load.after_view(starts.starter());
    page.chosen = std::optional<int>{2};
    page.load.after_view(starts.starter());

    virtual_time time;

    REQUIRE(quoll::test::run(time, starts.run_all()) == 0);
    CHECK(loads.lines == std::vector<std::string>{"start 2", "end 2"});
    CHECK(std::get<0>(*page.text) == "text 2");
  }

  SECTION("none") {
    bare_page page;
    kept_starts starts;

    page.load.after_view(starts.starter());
    page.chosen = std::optional<int>{};
    page.load.after_view(starts.starter());

    virtual_time time;

    REQUIRE(quoll::test::run(time, starts.run_all()) == 0);
    CHECK(loads.lines.empty());
    CHECK(shown_of(*page.text) == "nothing");
  }
}

TEST_CASE("effect: a run that fails leaves why in its target, and reload runs it again, why gone as it starts",
          "[ui][effect]") {
  loads = {};
  loads.failing = {1};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string failed;
  bool cleared = false;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    failed = shown.html();

    loads.failing.clear();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    auto const &[value, loading, error] = *instance.component().text;

    cleared = loading && error == nullptr;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed.contains("<p>failed</p>"));
  CHECK(cleared);
  CHECK(shown.html().contains("<p>text 1</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "start 1", "end 1"});
}

TEST_CASE("effect: a target with no error leaves a failure to the app's on_error", "[ui][effect]") {
  loads = {};
  loads.failing = {2};

  virtual_time time;
  memory_renderer const shown;
  std::vector<std::string> failures;
  test_app<plain_page> instance{time.get_executor(), shown, {.on_error = [&failures](std::exception_ptr failure) {
                                  failures.push_back(what_of(failure));
                                }}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().next = 2;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failures == std::vector<std::string>{"no text 2"});
  CHECK(shown.html().contains("<p>text 1</p>"));
}

TEST_CASE("effect: while a change runs, its target keeps the values before, as React shows the state before",
          "[ui][effect]") {
  loads = {};
  loads.delays = {{2, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  bool kept = false;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().next = 2;
    co_await settle();

    auto const &[value, loading, error] = *instance.component().text;

    kept = loading && value == "text 1";
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(kept);
  CHECK(shown.html().contains("<p>text 2</p>"));
}

TEST_CASE("effect: a reload after a change runs once, the view after it running it no more", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().next = 2;
    instance.component().load.reload();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1", "start 2", "end 2"});
}

TEST_CASE("effect: reload with a change in the same turn runs the latest only", "[ui][effect]") {
  loads = {};
  loads.failing = {1};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    loads.failing.clear();

    // A click on again, then a change before the view they ask for: one run, for the value the state holds then.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::yield();
    instance.component().next = 2;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>text 2</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "start 2", "end 2"});
}

TEST_CASE("effect: a function that throws before its task fails the run", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<checked_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().next = -1;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  auto const &error = std::get<2>(*instance.component().text);

  CHECK(shown.html().contains("<p>failed</p>"));
  REQUIRE(error != nullptr);
  CHECK_THROWS_AS(std::rethrow_exception(error), std::invalid_argument);
}

TEST_CASE("effect: on an optional state, runs only while it holds a value", "[ui][effect]") {
  loads = {};
  loads.delays = {{2, 50ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<choosing_page> instance{time.get_executor(), shown};
  std::string before;
  std::string chosen;
  std::string emptied;

  auto const act = [&]() -> task<> {
    co_await settle();
    before = shown.html();

    instance.component().chosen = std::optional<int>{1};
    co_await quoll::async::sleep_for(5ms);
    chosen = shown.html();

    // Emptied while a run is under way: it is stopped, loading no more, what the target held before kept.
    instance.component().chosen = std::optional<int>{2};
    co_await settle();
    instance.component().chosen = std::optional<int>{};
    co_await quoll::async::sleep_for(100ms);
    emptied = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before.contains("<p>nothing</p>"));
  CHECK(chosen.contains("<p>text 1</p>"));
  CHECK(emptied.contains("<p>text 1</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1", "start 2"});
  CHECK(loads.stopped == 1);
}

TEST_CASE("effect: on several states, a change of either runs it, its loads written at once", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<both_page> instance{time.get_executor(), shown};
  std::string first;
  std::string second;
  std::string failed;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    first = shown.html();

    instance.component().right = std::string{"b"};
    co_await quoll::async::sleep_for(5ms);
    second = shown.html();

    // One load of two fails: the run fails, the values before kept.
    loads.failing = {102};
    instance.component().left = 2;
    co_await quoll::async::sleep_for(5ms);
    failed = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first.contains("<p>text 1|text 101 a</p>"));
  CHECK(second.contains("<p>text 1|text 101 b</p>"));
  CHECK(failed.contains("<p>text 1|text 101 b failed</p>"));
}

TEST_CASE("effect: on no state, runs once", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<once_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().other = 1;
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>text 5 1</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 5", "end 5"});
}

TEST_CASE("effect: one of a component of a view shows what it gave", "[ui][effect]") {
  loads = {};
  loads.delays = {{7, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<panel_page> instance{time.get_executor(), shown};
  std::string running;
  std::string given;

  auto const act = [&]() -> task<> {
    co_await settle();
    running = shown.html();
    co_await quoll::async::sleep_for(20ms);
    given = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(running.contains("<p>running</p>"));
  CHECK(given.contains("<p>text 7</p>"));
}

TEST_CASE("effect: the run of a component that goes is stopped with it", "[ui][effect]") {
  loads = {};
  loads.delays = {{7, 50ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<panel_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await quoll::async::sleep_for(100ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loads.lines == std::vector<std::string>{"start 7"});
  CHECK(loads.stopped == 1);
}

TEST_CASE("effect: with no target, a change stops the run before, as a debounce", "[ui][effect]") {
  searched.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app<searching_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();

    for (auto const *typed : {"a", "ab", "abc"}) {
      instance.dispatch(shown.find("input"), "input", typed);
      co_await quoll::async::sleep_for(100ms);
    }

    co_await quoll::async::sleep_for(500ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(searched == std::vector<std::string>{"abc"});
}

TEST_CASE("effect: no function is refused as it is made", "[ui][effect]") {
  using loader = quoll::async::task<std::string> (*)(int);

  state<int> dependency;
  state<text_target> text;

  CHECK_THROWS_AS((effect{dependency, loader{}, text}), std::invalid_argument);
}

TEST_CASE("effect: one started as its app stops at once does not run", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};

  // Before the executor runs what the first view started: unmounted() has run, and the effect does nothing.
  auto const start_then_stop = [&]() -> task<int> {
    instance.start();
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
  CHECK(loads.lines.empty());
}

TEST_CASE("effect: one that could not start ends on that failure, which reload tries again", "[ui][effect]") {
  // Its run yields rather than wait on a timer, which this executor does not offer.
  loads = {};
  loads.stubborn = {1};

  virtual_time time;
  memory_renderer const shown;
  bool fail = false;
  std::vector<std::string> failures;
  test_app<other_page> instance{
      failing_executor{time.get_executor(), &fail}, shown, {.on_error = [&failures](std::exception_ptr failure) {
        failures.push_back(what_of(failure));
      }}};
  std::string failed_kept;
  std::string other;

  auto const act = [&]() -> task<int> {
    // Its first view shows, and its run cannot be spawned.
    fail = true;
    instance.start();
    fail = false;

    // Its target holds the failure, which a render to show could not be asked for either, as the executor refused that
    // too.
    auto const &error = std::get<2>(*instance.component().text);

    failed_kept = error ? what_of(error) : "";

    // A view for something else does not try it again, which would fail each time a lack lasts.
    instance.component().other = 1;
    co_await quoll::async::sleep_for(1ms);
    other = shown.html();

    instance.component().load.reload();
    co_await quoll::async::sleep_for(5ms);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, act()) == 0);
  CHECK(failures.empty());
  CHECK(failed_kept == "no more");
  CHECK(other.contains("failed 1"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1"});
  CHECK(shown.html().contains("text 1 1"));
}

TEST_CASE("effect: a view whose change cannot be recorded stops the run before, and waits for reload", "[ui][effect]") {
  loads = {};

  touchy_key_page page;
  kept_starts starts;

  // The run for 1 queued; the view of 2 fails as it copies the key: the run for 1 writes nothing.
  page.load.after_view(starts.starter());
  page.key = touchy_key{2};

  {
    throwing_keys const copies{key_copy_throws};

    page.load.after_view(starts.starter());
  }

  virtual_time time;

  REQUIRE(quoll::test::run(time, starts.run_all()) == 0);
  CHECK(loads.lines.empty());

  auto const &[value, loading, error] = *page.text;

  CHECK(value.empty());
  CHECK_FALSE(loading);
  REQUIRE(error != nullptr);
  CHECK(what_of(error) == "no copy");

  // A view after it runs nothing, as a failure to start waits for reload().
  page.load.after_view(starts.starter());
  CHECK(starts.started.empty());

  page.load.reload();
  page.load.after_view(starts.starter());

  REQUIRE(quoll::test::run(time, starts.run_all()) == 0);
  CHECK(std::get<0>(*page.text) == "text 2");
  CHECK(std::get<2>(*page.text) == nullptr);
}

TEST_CASE("effect: a run under way whose next view cannot compare its states is stopped, its failure thrown with no "
          "place for it",
          "[ui][effect]") {
  loads = {};
  loads.delays = {{1, 10ms}};

  touchy_key_loading_page page;
  kept_starts starts;
  bool loading_meanwhile = false;
  bool thrown = false;

  page.load.after_view(starts.starter());

  // The run for 1 begun, its target loading; a view whose comparison throws stops it.
  auto const view_fails = [&]() -> task<> {
    co_await quoll::async::yield();
    loading_meanwhile = std::get<1>(*page.text);

    throwing_keys const compares{key_compare_throws};

    try {
      page.load.after_view(starts.starter());
    } catch (std::runtime_error const &) {
      thrown = true;
    }
  };

  auto const act = [&]() -> task<int> {
    co_await quoll::async::when_all(std::move(starts.started.front()), view_fails());
    co_return 0;
  };

  virtual_time time;

  REQUIRE(quoll::test::run(time, act()) == 0);
  CHECK(loading_meanwhile);
  CHECK(thrown);
  CHECK(loads.lines == std::vector<std::string>{"start 1"});
  CHECK(loads.stopped == 1);
  CHECK(std::get<0>(*page.text).empty());
  CHECK_FALSE(std::get<1>(*page.text));
}
