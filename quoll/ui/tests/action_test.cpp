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
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/action.hpp>
#include <quoll/ui/resource.hpp>
#include <quoll/ui/run_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
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

task<int> doubled_for(std::string key, int value) {
  runs.lines.push_back(key + " start " + std::to_string(value));
  co_await quoll::async::sleep_for(10ms);
  runs.lines.push_back(key + " end " + std::to_string(value));
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

// Runs its action with the value it is given, on a click, and tells what each run gave.
template <overlap Policy>
struct running_page {
  state<int> next{1};
  action<int(int), Policy> run{doubled};

  element view() {
    auto const go = [this]() -> task<> {
      auto const value = *next;
      auto const result = co_await run.run(value);

      runs.lines.push_back("gave " + std::to_string(value) + " " + told(result));
    };

    auto const label = run.pending() ? "waits" : "go";

    return div({button(label) | on_click(go)});
  }
};

struct keyed_page {
  keyed_action<std::string, int(int)> saves{doubled_for};

  element view() { return div({}); }
};

struct restart_keyed_page {
  keyed_action<std::string, int(int), overlap::restart> saves{doubled_for};

  element view() { return div({}); }
};

// A result whose copy throws once asked to, as one that allocates may.
bool touchy_throws = false;

struct touchy {
  int value = 0;

  explicit touchy(int given) : value(given) {}

  touchy(touchy const &other) : value(other.value) {
    if (touchy_throws) {
      throw std::runtime_error{"no room"};
    }
  }

  touchy(touchy &&) noexcept = default;
  touchy &operator=(touchy const &) = default;
  touchy &operator=(touchy &&) noexcept = default;
  ~touchy() = default;
};

task<touchy> touchy_of(int value) {
  co_await quoll::async::sleep_for(1ms);
  co_return touchy{value};
}

struct touchy_page {
  action<touchy(int)> run{touchy_of};

  element view() { return div({}); }
};

struct timed_page {
  action<int(int)> run{doubled};

  element view() { return div({}); }
};

struct timed_restart_page {
  action<int(int), overlap::restart> run{doubled};

  element view() { return div({}); }
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
  CHECK(instance.component().run.result().value() == 2);
  CHECK_FALSE(instance.component().run.pending());
  CHECK(shown.html().contains("<button>go</button>"));
}

TEST_CASE("action: with overlap::restart, a call stops the run under way, which is replaced", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<running_page<overlap::restart>> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(runs.lines == std::vector<std::string>{"start 1", "start 2", "gave 1 replaced", "end 2", "gave 2 yes"});
  CHECK(instance.component().run.result().value() == 4);
}

TEST_CASE("action: a run that fails gives why, which its result keeps while the next is under way", "[ui][action]") {
  runs = {};
  runs.failing = {2};

  virtual_time time;
  memory_renderer const shown;
  test_app<running_page<overlap::refuse>> instance{time.get_executor(), shown};
  bool failed_after_second = false;
  bool failure_while_third = false;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);

    instance.component().next = 2;
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
    failed_after_second = !instance.component().run.result().has_value();

    instance.component().next = 3;
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    failure_while_third = instance.component().run.pending() && !instance.component().run.result().has_value();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed_after_second);
  CHECK(failure_while_third);
  CHECK(runs.lines == std::vector<std::string>{"start 1", "end 1", "gave 1 yes", "start 2", "gave 2 no", "start 3",
                                               "end 3", "gave 3 yes"});
  CHECK(instance.component().run.result().value() == 6);
}

TEST_CASE("keyed_action: each key once at a time, the others left alone", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<keyed_page> instance{time.get_executor(), shown};
  std::vector<bool> answers;
  bool a_pending = false;
  bool b_pending = false;

  auto const act = [&]() -> task<> {
    auto &saves = instance.component().saves;

    auto const save = [&](std::string key, int value) -> task<> {
      answers.push_back((co_await saves.run(std::move(key), value)).has_value());
    };

    co_await quoll::async::when_all(save("a", 1), save("a", 2), save("b", 3), [&]() -> task<> {
      co_await settle();
      a_pending = saves.pending("a");
      b_pending = saves.pending("b");
    }());
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  auto const &saves = instance.component().saves;

  CHECK(a_pending);
  CHECK(b_pending);
  CHECK(answers == std::vector<bool>{false, true, true});
  CHECK(saves.result("a").value() == 2);
  CHECK(saves.result("b").value() == 6);
  CHECK_FALSE(saves.has_result("c"));
  CHECK_THROWS_AS(saves.result("c"), std::logic_error);
  CHECK_FALSE(saves.pending("a"));
}

TEST_CASE("action: of nothing, gives whether it ended, and keeps why it failed", "[ui][action]") {
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
  CHECK_FALSE(instance.component().touch.result().has_value());
}

TEST_CASE("keyed_action: with overlap::restart, a call for a key stops that key's run only", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<restart_keyed_page> instance{time.get_executor(), shown};
  std::vector<bool> answers;

  auto const act = [&]() -> task<> {
    auto &saves = instance.component().saves;

    auto const save = [&](std::string key, int value) -> task<> {
      answers.push_back((co_await saves.run(std::move(key), value)).has_value());
    };

    co_await quoll::async::when_all(save("a", 1), save("a", 2), save("b", 3));
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);

  auto const &saves = instance.component().saves;

  CHECK(answers == std::vector<bool>{false, true, true});
  CHECK(saves.result("a").value() == 4);
  CHECK(saves.result("b").value() == 6);
}

TEST_CASE("action: a run its caller stops, as a timeout does, is under way no more", "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<timed_page> instance{time.get_executor(), shown};
  bool timed_out = false;
  bool pending_after = true;
  bool again = false;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    try {
      static_cast<void>(co_await quoll::async::with_timeout(5ms, run.run(1)));
    } catch (std::system_error const &failure) {
      timed_out = failure.code() == std::errc::timed_out;
    }

    pending_after = run.pending();
    again = (co_await run.run(2)).has_value();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(timed_out);
  CHECK_FALSE(pending_after);
  CHECK(again);
  CHECK(instance.component().run.result().value() == 4);
}

TEST_CASE("action: no function is refused as it is made", "[ui][action]") {
  using function = task<int> (*)(int);

  CHECK_THROWS_AS((action<int(int)>{function{}}), std::invalid_argument);
  CHECK_THROWS_AS((keyed_action<int, int(int)>{nullptr}), std::invalid_argument);
}

namespace {

task<int> yields_once(int value) {
  co_await quoll::async::yield();
  co_return value;
}

task<std::string> yields_text(int) {
  co_await quoll::async::yield();
  co_return "text";
}

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

TEST_CASE("keyed_action: destroyed while a run is under way is asserted", "[.assert]") {
  std::optional<keyed_action<int, int()>> held;

  held.emplace(yields_once);
  destroyed_while_under_way(held, [](auto &saves) { return saves.run(1); });
}

TEST_CASE("resource: destroyed while a load is under way is asserted", "[.assert]") {
  std::optional<resource<int, std::string>> held;

  held.emplace(yields_text);
  destroyed_while_under_way(held, [](auto &text) { return text.load(1); });
}

TEST_CASE("action: a run its caller stops once a later one took its place leaves the later one under way",
          "[ui][action]") {
  runs = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<timed_restart_page> instance{time.get_executor(), shown};
  bool pending_meanwhile = false;
  bool second_gave = false;

  auto const act = [&]() -> task<> {
    auto &run = instance.component().run;

    // The first run, its caller stopping it at 5 ms; the second, from 5 ms too, replaces it at the same instant:
    // whichever stop comes first, the second is under way until it ends, at 15 ms.
    auto const first = [&]() -> task<> {
      try {
        static_cast<void>(co_await quoll::async::with_timeout(5ms, run.run(1)));
      } catch (std::system_error const &) {
      }
    };

    auto const second = [&]() -> task<> {
      co_await quoll::async::sleep_for(5ms);
      second_gave = (co_await run.run(2)).has_value();
    };

    auto const watch = [&]() -> task<> {
      co_await quoll::async::sleep_for(7ms);
      pending_meanwhile = run.pending();
    };

    co_await quoll::async::when_all(first(), second(), watch());
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(pending_meanwhile);
  CHECK(second_gave);
  CHECK(instance.component().run.result().value() == 4);
}

TEST_CASE("action: a result that cannot be kept is this run's failure, given rather than thrown", "[ui][action]") {
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

  auto const &run = instance.component().run;

  CHECK(first);
  CHECK(second == "no");
  CHECK_FALSE(run.pending());
  REQUIRE_FALSE(run.result().has_value());
  CHECK_THROWS_AS(std::rethrow_exception(run.result().error()), std::runtime_error);
}
