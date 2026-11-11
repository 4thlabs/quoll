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
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/effect.hpp>
#include <quoll/ui/run_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
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

// What an effect shows, in words.
template <class Effect>
std::string shown_of(Effect const &loaded) {
  switch (loaded.status()) {
  case effect_status::none:
    return "nothing";
  case effect_status::running:
    return "running";
  case effect_status::stopped:
    return "stopped";
  case effect_status::done:
    break;
  }

  return loaded.result() ? *loaded.result() : "failed";
}

// Loads the text of the key it holds, as it changes; a click loads it again.
struct loading_page {
  state<int> next{1};
  effect<std::string> text{next, load_text};

  element view() {
    auto const again = [this]() -> task<> { static_cast<void>(co_await text.reload()); };

    return div({button("again") | on_click(again), p(shown_of(text))});
  }
};

// Loads with a loader that may throw before its task.
struct checked_page {
  state<int> next{1};
  effect<std::string> text{next, checked_text};

  element view() { return p(shown_of(text)); }
};

// Loads only once a key is chosen.
struct choosing_page {
  state<std::optional<int>> chosen;
  effect<std::string> text{chosen, load_text};

  element view() { return p(shown_of(text)); }
};

// A panel loading as it shows, which its page may take away.
struct loading_panel {
  state<int> key{7};
  effect<std::string> text{key, load_text};

  element view() { return p(shown_of(text)); }
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
  effect<std::string> text{next, load_text};

  element view() { return p(shown_of(text) + " " + std::to_string(*other)); }
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

// Searches for what is typed, a while after the last change: a debounce.
std::vector<std::string> searched;

struct searching_page {
  state<std::string> query;

  effect<> search{query, [](std::string text) -> task<> {
                    co_await quoll::async::sleep_for(300ms);
                    searched.push_back(std::move(text));
                  }};

  element view() { return input() | bind(query); }
};

} // namespace

TEST_CASE("effect: runs once its component first shows, shows that it runs, then what it gave", "[ui][effect]") {
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

TEST_CASE("effect: a run that fails keeps why, and reload runs it again", "[ui][effect]") {
  loads = {};
  loads.failing = {1};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string failed;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    failed = shown.html();

    loads.failing.clear();
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed.contains("<p>failed</p>"));
  CHECK(shown.html().contains("<p>text 1</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "start 1", "end 1"});
}

TEST_CASE("effect: the view a change makes shows it running, not what it gave for the value before", "[ui][effect]") {
  loads = {};
  loads.delays = {{2, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string changed;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().next = 2;

    // The render the change asked for runs, then this, before the run it starts.
    co_await quoll::async::yield();
    changed = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(changed.contains("<p>running</p>"));
  CHECK_FALSE(changed.contains("text 1"));
  CHECK(shown.html().contains("<p>text 2</p>"));
}

TEST_CASE("effect: reload forgets a failure as it starts, and a later change replaces it", "[ui][effect]") {
  loads = {};
  loads.failing = {1};
  loads.delays = {{1, 1ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string reloading;
  bool replaced = false;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    loads.failing.clear();
    loads.delays = {{1, 10ms}, {2, 1ms}};

    auto const reload = [&]() -> task<> {
      auto const result = co_await instance.component().text.reload();

      try {
        std::rethrow_exception(result.error());
      } catch (run_replaced const &) {
        replaced = true;
      } catch (...) {
      }
    };

    auto const change = [&]() -> task<> {
      co_await quoll::async::sleep_for(2ms);
      reloading = shown.html();
      instance.component().next = 2;
    };

    co_await quoll::async::when_all(reload(), change());
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(reloading.contains("<p>running</p>"));
  CHECK(replaced);
  CHECK(shown.html().contains("<p>text 2</p>"));
}

TEST_CASE("effect: a reload after a change runs once, the view after it running it no more", "[ui][effect]") {
  loads = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  bool gave = false;

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(5ms);
    instance.component().next = 2;
    gave = (co_await instance.component().text.reload()).has_value();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(gave);
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1", "start 2", "end 2"});
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

  auto const &text = instance.component().text;

  CHECK(shown.html().contains("<p>failed</p>"));
  REQUIRE(text.status() == effect_status::done);
  CHECK_THROWS_AS(std::rethrow_exception(text.result().error()), std::invalid_argument);
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

    // Emptied while a run is under way: it is stopped, and nothing is left.
    instance.component().chosen = std::optional<int>{2};
    co_await settle();
    instance.component().chosen = std::optional<int>{};
    co_await quoll::async::sleep_for(100ms);
    emptied = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before.contains("<p>nothing</p>"));
  CHECK(chosen.contains("<p>text 1</p>"));
  CHECK(emptied.contains("<p>nothing</p>"));
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1", "start 2"});
  CHECK(loads.stopped == 1);
  CHECK_THROWS_AS(instance.component().text.reload(), std::logic_error);
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

TEST_CASE("effect: a run its caller stops, as a timeout does, leaves it stopped", "[ui][effect]") {
  loads = {};
  loads.delays = {{1, 50ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  bool timed_out = false;

  auto const act = [&]() -> task<> {
    // Once its first run started, which the reload replaces.
    co_await settle();

    try {
      static_cast<void>(co_await quoll::async::with_timeout(5ms, instance.component().text.reload()));
    } catch (std::system_error const &failure) {
      timed_out = failure.code() == std::errc::timed_out;
    }

    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(timed_out);
  CHECK(instance.component().text.status() == effect_status::stopped);
  CHECK(shown.html().contains("<p>stopped</p>"));
  CHECK_THROWS_AS(instance.component().text.result(), std::logic_error);
}

TEST_CASE("effect: of nothing, a change stops the run before, as a debounce", "[ui][effect]") {
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
  CHECK(instance.component().search.status() == effect_status::done);
  CHECK(instance.component().search.result().has_value());
}

TEST_CASE("effect: no function is refused as it is made", "[ui][effect]") {
  using loader = quoll::async::task<std::string> (*)(int);

  state<int> dependency;

  CHECK_THROWS_AS((effect<std::string>{dependency, loader{}}), std::invalid_argument);
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

TEST_CASE("effect: one that could not start runs after the next view", "[ui][effect]") {
  // Its run yields rather than wait on a timer, which this executor does not offer.
  loads = {};
  loads.stubborn = {1};

  virtual_time time;
  memory_renderer const shown;
  bool fail = false;
  std::vector<std::string> failures;
  test_app<other_page> instance{
      failing_executor{time.get_executor(), &fail}, shown, {.on_error = [&failures](std::exception_ptr failure) {
        try {
          std::rethrow_exception(failure);
        } catch (std::exception const &error) {
          failures.emplace_back(error.what());
        }
      }}};

  auto const act = [&]() -> task<int> {
    // Its first view shows, and its run cannot be spawned.
    fail = true;
    instance.start();
    fail = false;

    // A view for something else: the effect tries again.
    instance.component().other = 1;
    co_await quoll::async::sleep_for(5ms);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, act()) == 0);
  CHECK(failures == std::vector<std::string>{"no more"});
  CHECK(loads.lines == std::vector<std::string>{"start 1", "end 1"});
  CHECK(shown.html().contains("text 1 1"));
}
