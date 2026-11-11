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

#include <quoll/async/await_context.hpp>
#include <quoll/async/detail/awaiting.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/allocations.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/action.hpp>
#include <quoll/ui/effect.hpp>
#include <quoll/ui/run_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
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

// The runs of the test running: which fail, and what each saw happen.
struct run_log {
  std::set<int> failing;
  std::vector<std::string> lines;
};

run_log runs;

// Takes 10 ms, and gives what it was given, doubled.
task<int> doubled(int value) {
  runs.lines.push_back("start " + std::to_string(value));
  co_await quoll::async::sleep_for(10ms);

  if (runs.failing.contains(value)) {
    throw std::runtime_error{"no " + std::to_string(value)};
  }

  runs.lines.push_back("end " + std::to_string(value));
  co_return value * 2;
}

task<> nothing_much(int value) {
  co_await quoll::async::sleep_for(1ms);

  if (runs.failing.contains(value)) {
    throw std::runtime_error{"no"};
  }
}

// What a run's result says, in a word.
template <class Result>
std::string told(Result const &result) {
  if (result) {
    return "yes";
  }

  try {
    std::rethrow_exception(result.error());
  } catch (run_refused const &) {
    return "refused";
  } catch (run_replaced const &) {
    return "replaced";
  } catch (...) {
    return "no";
  }
}

// Whether a run is under way, and why the last failed.
using flags = std::tuple<bool, std::exception_ptr>;

// Runs its action with the value it is given, on a click, and tells what each run gave.
template <overlap Policy>
struct running_page {
  state<int> next{1};
  state<flags> running;
  action<int(int), Policy> run{doubled, running};

  element view() {
    auto const go = [this]() -> task<> {
      auto const value = *next;
      auto const result = co_await run.run(value);

      runs.lines.push_back("gave " + std::to_string(value) + " " + told(result));
    };

    auto const label = std::get<0>(*running) ? "waits" : "go";

    return div({button(label) | on_click(go)});
  }
};

// Its target holds what it gave too, a copy: its caller is given the run's own.
struct valued_page {
  state<std::tuple<int, bool, std::exception_ptr>> doubled_value;
  action<int(int)> run{doubled, doubled_value};

  element view() { return p(std::to_string(std::get<0>(*doubled_value))); }
};

// A result whose copy throws once asked to, as one that allocates may.
bool touchy_throws = false;

struct touchy {
  int value = 0;

  touchy() = default;

  explicit touchy(int given) : value(given) {}

  touchy(touchy const &other) : value(other.value) {
    if (touchy_throws) {
      throw std::runtime_error{"no room"};
    }
  }

  touchy(touchy &&) noexcept = default;

  touchy &operator=(touchy const &other) {
    if (touchy_throws) {
      throw std::runtime_error{"no room"};
    }

    value = other.value;
    return *this;
  }

  touchy &operator=(touchy &&) noexcept = default;
  ~touchy() = default;
};

task<touchy> touchy_of(int value) {
  co_await quoll::async::sleep_for(1ms);
  co_return touchy{value};
}

struct touchy_page {
  state<std::tuple<touchy, std::exception_ptr>> kept;
  action<touchy(int)> run{touchy_of, kept};

  element view() { return div({}); }
};

task<std::tuple<int, touchy>> pair_of(int value) {
  co_await quoll::async::sleep_for(1ms);
  co_return {value, touchy{value}};
}

// Two values, the second's copy may throw: written both, or neither.
struct pair_page {
  state<std::tuple<int, touchy, std::exception_ptr>> kept;
  action<std::tuple<int, touchy>(int)> run{pair_of, kept};

  element view() { return div({}); }
};

struct timed_page {
  state<bool> busy;
  action<int(int)> run{doubled, busy};

  element view() { return div({}); }
};

struct timed_restart_page {
  state<bool> busy;
  action<int(int), overlap::restart> run{doubled, busy};

  element view() { return div({}); }
};

// Runs its action as it first shows, and goes as its page asks.
struct running_panel {
  state<bool> busy;
  action<int(int)> run{doubled, busy};

  task<> mounted() { static_cast<void>(co_await run.run(1)); }

  element view() { return div({}); }
};

struct panel_page {
  state<bool> shows{true};

  element view() {
    auto const panel = [] { return component<running_panel>(); };

    return div({when(*shows, panel)});
  }
};

struct void_page {
  action<void(int)> touch{nothing_much};

  element view() { return div({}); }
};

} // namespace

TEST_CASE("action: one at a time, a call while one is under way refused at once", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<running_page<overlap::refuse>> instance{time.get_executor(), shown};
  std::string waiting;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    waiting = shown.html();

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(waiting.contains("<button>waits</button>"));
  CHECK(runs.lines == std::vector<std::string>{"start 1", "gave 2 refused", "end 1", "gave 1 yes"});
  CHECK(shown.html().contains("<button>go</button>"));
}

TEST_CASE("action: with overlap::restart, a call stops the run under way, which is replaced", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<running_page<overlap::restart>> instance{time.get_executor(), shown};
  std::string restarted;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    restarted = shown.html();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(runs.lines == std::vector<std::string>{"start 1", "start 2", "gave 1 replaced", "end 2", "gave 2 yes"});
  CHECK(restarted.contains("<button>waits</button>"));
  CHECK(shown.html().contains("<button>go</button>"));
}

TEST_CASE("action: a run that fails leaves why in its target, gone as the next starts", "[ui][action]") {
  runs = {};
  runs.failing = {2};

  virtual_time time;
  memory_renderer const shown;
  test_app<running_page<overlap::refuse>> instance{time.get_executor(), shown};
  bool failed_after_second = false;
  bool cleared_on_third = false;

  auto const act = [&]() -> task<> {
    auto const &running = instance.component().running;

    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
    failed_after_second = !std::get<0>(*running) && std::get<1>(*running) != nullptr;

    instance.component().next = 3;
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    cleared_on_third = std::get<0>(*running) && std::get<1>(*running) == nullptr;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed_after_second);
  CHECK(cleared_on_third);
  CHECK(runs.lines == std::vector<std::string>{"start 1", "end 1", "gave 1 yes", "start 2", "gave 2 no", "start 3",
                                               "end 3", "gave 3 yes"});
}

TEST_CASE("action: a target with a value holds a copy of what its caller is given", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<valued_page> instance{time.get_executor(), shown};
  int given = 0;

  auto const act = [&]() -> task<> {
    auto const result = co_await instance.component().run.run(2);

    given = result.value_or(0);
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(given == 4);
  CHECK(shown.html().contains("<p>4</p>"));
}

TEST_CASE("action: of nothing, with no target, gives whether it ended", "[ui][action]") {
  runs = {};
  runs.failing = {2};

  virtual_time time;
  memory_renderer const shown;
  test_app<void_page> instance{time.get_executor(), shown};
  bool first = false;
  bool second = true;

  auto const act = [&]() -> task<> {
    first = (co_await instance.component().touch.run(1)).has_value();
    second = (co_await instance.component().touch.run(2)).has_value();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first);
  CHECK_FALSE(second);
}

TEST_CASE("action: a run its caller stops, as a timeout does, is under way no more", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<timed_page> instance{time.get_executor(), shown};
  bool timed_out = false;
  bool busy_after = true;
  bool again = false;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    try {
      static_cast<void>(co_await quoll::async::with_timeout(5ms, run.run(1)));
    } catch (std::system_error const &failure) {
      timed_out = failure.code() == std::errc::timed_out;
    }

    busy_after = *instance.component().busy;
    again = (co_await run.run(2)).has_value();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(timed_out);
  CHECK_FALSE(busy_after);
  CHECK(again);
  CHECK_FALSE(*instance.component().busy);
}

TEST_CASE("action: no function is refused as it is made", "[ui][action]") {
  using function = task<int> (*)(int);

  state<bool> busy;

  CHECK_THROWS_AS((action<int(int)>{function{}}), std::invalid_argument);
  CHECK_THROWS_AS((action<int(int)>{function{}, busy}), std::invalid_argument);
}

namespace {

task<int> yields_once(int value) {
  co_await quoll::async::yield();
  co_return value;
}

task<> yields_nothing(int) { co_await quoll::async::yield(); }

// Starts what `start` gives on a loop that never runs, so that it stays under way, then destroys `holder`'s object.
template <class Holder, class Start>
void destroyed_while_under_way(Holder &holder, Start start) {
#if defined(NDEBUG)
  // What the ctest takes for a build with nothing to check, however NDEBUG came to be defined.
  std::puts("quoll: assertions are compiled out");
#else
  quoll::async::run_loop loop;
  auto const executor = loop.get_executor();
  quoll::async::await_context const context{.executor = quoll::async::executor_ref{executor}, .stop_token = {}};
  auto running = start(*holder);

  // Through Quoll's own awaiting, as a program cannot start a task by hand: it waits on a yield the loop never runs.
  quoll::async::detail::awaiting::suspend(running, std::noop_coroutine(), context).resume();
  holder.reset();
#endif
}

} // namespace

TEST_CASE("action: destroyed while a run is under way is asserted", "[.assert]") {
  std::optional<action<int(int)>> held;

  held.emplace(yields_once);
  destroyed_while_under_way(held, [](auto &run) { return run.run(1); });
}

TEST_CASE("effect: destroyed while a run is under way is asserted", "[.assert]") {
  state<int> dependency{1};
  std::optional<effect> held;

  held.emplace(dependency, yields_nothing);

  // Its run, as its component's app would start it after a view.
  destroyed_while_under_way(held, [](auto &text) {
    std::optional<task<>> started;

    text.after_view({.owner = &started, .start = [](void *owner, task<> work) {
                       static_cast<std::optional<task<>> *>(owner)->emplace(std::move(work));
                     }});

    return std::move(*started);
  });
}

TEST_CASE("action: a run its caller stops once a later one took its place leaves the later one under way",
          "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<timed_restart_page> instance{time.get_executor(), shown};
  std::stop_source caller;
  bool busy_meanwhile = false;
  bool second_gave = false;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    // The first run, under a stop of its caller's that the test asks for.
    auto const stoppable = [&]() -> task<> {
      using result = action<int(int), overlap::restart>::result_type;

      static_cast<void>(co_await quoll::async::detail::stop_scope<result>(run.run(1), caller.get_token()));
    };

    auto const first = [&]() -> task<> { static_cast<void>(co_await quoll::async::stopped_as_optional(stoppable())); };

    // In one turn, before the first resumes: its caller stops it, and the second takes its place, so that the first
    // resumes stopped by both, a later run under way.
    auto const second = [&]() -> task<> {
      co_await quoll::async::sleep_for(1ms);
      caller.request_stop();
      second_gave = (co_await run.run(2)).has_value();
    };

    auto const watch = [&]() -> task<> {
      co_await quoll::async::sleep_for(5ms);
      busy_meanwhile = *instance.component().busy;
    };

    co_await quoll::async::when_all(first(), second(), watch());
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(busy_meanwhile);
  CHECK(second_gave);
  CHECK_FALSE(*instance.component().busy);
}

TEST_CASE("action: a value its target cannot take is this run's failure, given rather than thrown", "[ui][action]") {
  runs = {};
  touchy_throws = false;

  virtual_time time;
  memory_renderer const shown;
  test_app<touchy_page> instance{time.get_executor(), shown};
  bool first = false;
  std::string second;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    first = (co_await run.run(1)).has_value();
    touchy_throws = true;

    auto const given = co_await run.run(2);

    touchy_throws = false;
    second = told(given);

    if (!given) {
      CHECK_THROWS_AS(std::rethrow_exception(given.error()), std::runtime_error);
    }
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  auto const &[kept, error] = *instance.component().kept;

  CHECK(first);
  CHECK(second == "no");
  CHECK(kept.value == 1);
  REQUIRE(error != nullptr);
  CHECK_THROWS_AS(std::rethrow_exception(error), std::runtime_error);
}

TEST_CASE("action: a run whose second value cannot be made writes none of them", "[ui][action]") {
  runs = {};
  touchy_throws = false;

  virtual_time time;
  memory_renderer const shown;
  test_app<pair_page> instance{time.get_executor(), shown};
  bool second_failed = false;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    static_cast<void>(co_await run.run(1));
    touchy_throws = true;
    second_failed = !(co_await run.run(2)).has_value();
    touchy_throws = false;
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  auto const &[number, kept, error] = *instance.component().kept;

  CHECK(second_failed);
  CHECK(number == 1);
  CHECK(kept.value == 1);
  CHECK(error != nullptr);
}

TEST_CASE("action: a component that goes stops its runs", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<panel_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(runs.lines == std::vector<std::string>{"start 1"});
}

TEST_CASE("action: a lack of memory at any step of a run is its failure, given to its caller and its target",
          "[ui][action]") {
  // Each allocation the run makes, in turn, fails: whichever it is, the run's await gives a std::expected, and a run
  // that failed leaves its target not loading.
  for (long before = 0; before < 40; ++before) {
    runs = {};

    virtual_time time;
    memory_renderer const shown;
    test_app<running_page<overlap::refuse>> instance{time.get_executor(), shown};
    bool escaped = false;
    // Whether an allocation failed: one does once, the count back below zero.
    bool injected = false;

    auto const act = [&]() -> task<> {
      co_await settle();

      auto &run = instance.component().run;
      auto running = run.run(1);

      quoll::test::allocations_before_failing = before;

      try {
        static_cast<void>(co_await std::move(running));
      } catch (...) {
        escaped = true;
      }

      injected = quoll::test::allocations_before_failing < 0;
      quoll::test::allocations_before_failing = -1;
      co_await settle();
    };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
    INFO("allocations before failing: " << before);
    CHECK_FALSE(escaped);
    CHECK_FALSE(std::get<0>(*instance.component().running));

    if (!injected) {
      break;
    }
  }
}
