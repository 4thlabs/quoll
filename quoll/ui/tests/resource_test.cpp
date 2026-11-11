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
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/resource.hpp>
#include <quoll/ui/run_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <map>
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

// The loads of the test running: how long each key takes, which fail, and how many started, ended or were stopped.
struct load_log {
  std::map<int, std::chrono::milliseconds> delays;
  std::set<int> failing;
  // Keys whose load ignores its stop: it yields to the executor rather than wait on a timer, its resumption queued
  // before the stop is asked for, and ends with no other wait, as an answer already on its way does.
  std::set<int> stubborn;
  int started = 0;
  int ended = 0;
  int stopped = 0;
};

load_log loads;

task<std::string> load_text(int key) {
  ++loads.started;

  // Destroyed without having ended: the load was stopped, and its frame destroyed from the top.
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

  auto const delay = loads.delays.find(key);

  if (loads.stubborn.contains(key)) {
    co_await quoll::async::yield();
    co_await quoll::async::yield();
  } else {
    co_await quoll::async::sleep_for(delay == loads.delays.end() ? 1ms : delay->second);
  }

  if (loads.failing.contains(key)) {
    throw std::runtime_error{"no text " + std::to_string(key)};
  }

  watched.ended = true;
  ++loads.ended;
  co_return "text " + std::to_string(key);
}

// Throws for a key below zero before giving a task, as a function checking its argument first does.
task<std::string> checked_text(int key) {
  if (key < 0) {
    throw std::invalid_argument{"no such key"};
  }

  return load_text(key);
}

// Whether a load's failure is that a later one took its place.
bool is_replaced(std::exception_ptr const &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (run_replaced const &) {
    return true;
  } catch (...) {
    return false;
  }
}

// What a resource shows, in words.
template <class Resource>
std::string shown_of(Resource const &loaded) {
  switch (loaded.status()) {
  case load_status::none:
    return "nothing";
  case load_status::loading:
    return "loading";
  case load_status::stopped:
    return "stopped";
  case load_status::done:
    break;
  }

  return loaded.result() ? *loaded.result() : "failed";
}

// Loads the text of the key it is given, on a click.
struct loading_page {
  state<int> next{1};
  resource<int, std::string> text{load_text};
  // What each handler's load gave, as it went on past it, and how many a later one replaced.
  std::vector<bool> ended;
  int replaced = 0;

  element view() {
    auto const load = [this]() -> task<> {
      auto const result = co_await text.load(*next);

      ended.push_back(result.has_value());
      replaced += result ? 0 : is_replaced(result.error()) ? 1 : 0;
    };

    return div({button("load") | on_click(load), p(shown_of(text))});
  }
};

// Loads with a loader that may throw before its task.
struct checked_page {
  state<int> next{1};
  resource<int, std::string> text{checked_text};
  std::vector<bool> ended;
  int replaced = 0;

  element view() {
    auto const load = [this]() -> task<> {
      auto const result = co_await text.load(*next);

      ended.push_back(result.has_value());
      replaced += result ? 0 : is_replaced(result.error()) ? 1 : 0;
    };

    return div({button("load") | on_click(load), p(shown_of(text))});
  }
};

// A panel loading as it shows, which its page may take away.
struct loading_panel {
  resource<int, std::string> text{load_text};

  task<> mounted() { static_cast<void>(co_await text.load(7)); }

  element view() { return p(shown_of(text)); }
};

struct panel_page {
  state<bool> shows{true};

  element view() {
    auto const panel = [] { return component<loading_panel>(); };

    return div({when(*shows, panel)});
  }
};

} // namespace

TEST_CASE("resource: shows that it loads, then what it loaded, for the key given last", "[ui][resource]") {
  loads = {};
  loads.delays = {{1, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string loading;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    loading = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loading.contains("<p>loading</p>"));
  CHECK(shown.html().contains("<p>text 1</p>"));
  CHECK(instance.component().text.key() == 1);
  CHECK(instance.component().ended == std::vector<bool>{true});
}

TEST_CASE("resource: loading again stops the load before, whose answer writes nothing and which is replaced",
          "[ui][resource]") {
  // The first load answers after the second.
  loads = {};
  loads.delays = {{1, 10ms}, {2, 1ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>text 2</p>"));
  CHECK(loads.started == 2);
  CHECK(loads.ended == 1);
  CHECK(loads.stopped == 1);
  // The first handler goes on, told a later load took its place.
  CHECK(instance.component().ended == std::vector<bool>{false, true});
  CHECK(instance.component().replaced == 1);
  CHECK(instance.component().text.key() == 2);
}

TEST_CASE("resource: a load that fails keeps why, and its caller goes on", "[ui][resource]") {
  loads = {};
  loads.failing = {1};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>failed</p>"));
  REQUIRE(instance.component().text.status() == load_status::done);
  REQUIRE_FALSE(instance.component().text.result().has_value());
  CHECK_THROWS_AS(std::rethrow_exception(instance.component().text.result().error()), std::runtime_error);
  CHECK(instance.component().ended == std::vector<bool>{false});
}

TEST_CASE("resource: a load after a failure forgets it as it starts", "[ui][resource]") {
  loads = {};
  loads.delays = {{2, 10ms}};
  loads.failing = {1};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string failed;
  std::string again;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(5ms);
    failed = shown.html();

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    again = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed.contains("<p>failed</p>"));
  CHECK(again.contains("<p>loading</p>"));
  CHECK(shown.html().contains("<p>text 2</p>"));
}

TEST_CASE("resource: one of a component of a view shows what it loaded", "[ui][resource]") {
  loads = {};
  loads.delays = {{7, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<panel_page> instance{time.get_executor(), shown};
  std::string loading;

  auto const act = [&]() -> task<> {
    co_await settle();
    loading = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loading.contains("<p>loading</p>"));
  CHECK(shown.html().contains("<p>text 7</p>"));
}

TEST_CASE("resource: the load of a component that goes is stopped with it", "[ui][resource]") {
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
  CHECK(loads.started == 1);
  CHECK(loads.stopped == 1);
  CHECK(loads.ended == 0);
}

TEST_CASE("resource: a load stopped by a later one that ends anyway writes nothing", "[ui][resource]") {
  // The first load ignores its stop, and ends after the second has begun.
  loads = {};
  loads.stubborn = {1};
  loads.delays = {{2, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  std::string meanwhile;

  auto const act = [&]() -> task<> {
    // The first handler runs, and its load waits on its first yield; the second load starts during its second.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::yield();
    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    meanwhile = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loads.ended == 2);
  CHECK(meanwhile.contains("<p>loading</p>"));
  CHECK(shown.html().contains("<p>text 2</p>"));
  CHECK(instance.component().ended == std::vector<bool>{false, true});
}

TEST_CASE("resource: a loader that throws before its task fails this load, the one before stopped", "[ui][resource]") {
  loads = {};
  loads.delays = {{1, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<checked_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    instance.component().next = -1;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("<p>failed</p>"));
  CHECK(instance.component().text.key() == -1);
  CHECK(loads.stopped == 1);
  CHECK(instance.component().ended == std::vector<bool>{false, false});
  CHECK_THROWS_AS(std::rethrow_exception(instance.component().text.result().error()), std::invalid_argument);
}

TEST_CASE("resource: no loader is refused as it is made", "[ui][resource]") {
  using loader = quoll::async::task<std::string> (*)(int);

  CHECK_THROWS_AS((resource<int, std::string>{loader{}}), std::invalid_argument);
  CHECK_THROWS_AS((resource<int, std::string>{std::function<task<std::string>(int)>{}}), std::invalid_argument);
}

TEST_CASE("resource: a load its caller stops, as a timeout does, leaves it stopped, for its key", "[ui][resource]") {
  loads = {};
  loads.delays = {{1, 50ms}};

  virtual_time time;
  memory_renderer const shown;
  test_app<loading_page> instance{time.get_executor(), shown};
  bool timed_out = false;

  auto const act = [&]() -> task<> {
    try {
      static_cast<void>(co_await quoll::async::with_timeout(5ms, instance.component().text.load(1)));
    } catch (std::system_error const &failure) {
      timed_out = failure.code() == std::errc::timed_out;
    }

    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(timed_out);
  CHECK(instance.component().text.status() == load_status::stopped);
  CHECK(instance.component().text.key() == 1);
  CHECK(shown.html().contains("<p>stopped</p>"));
  CHECK_THROWS_AS(instance.component().text.result(), std::logic_error);
}
