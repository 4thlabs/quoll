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
#include <quoll/async/when_all.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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

struct journal {
  std::vector<std::string> lines;
};

// A row asking its parent to save it, and saying once it has.
struct saving_row {
  struct props {
    std::string label;
    callback<void(std::string)> on_save;
    journal *log = nullptr;
  };

  element view(props const &given) {
    return button(given.label) | on_click([&given]() -> task<> {
             co_await given.on_save(given.label);
             given.log->lines.emplace_back("the row saw it saved");
           });
  }
};

// Saves after a minute, as a server would answer.
struct saving_page {
  state<bool> shows{true};
  journal log;

  element view() {
    return div({when(*shows, [this] {
      return component<saving_row>({.label = "a",
                                    .on_save = [this](std::string label) -> task<> {
                                      co_await quoll::async::sleep_for(1min);
                                      log.lines.push_back("saved " + label);
                                    },
                                    .log = &log});
    })});
  }
};

// A row asking its parent for a value.
struct asking_row {
  struct props {
    callback<int(int)> twice;
    journal *log = nullptr;
  };

  element view(props const &given) {
    return button("ask") | on_click([&given]() -> task<> {
             auto const answer = co_await given.twice(2);

             given.log->lines.push_back("got " + std::to_string(answer));
           });
  }
};

struct asking_page {
  journal log;

  element view() {
    return div({component<asking_row>({.twice = [](int value) { return value * 2; }, .log = &log})});
  }
};

// A row whose call fails, and which sees it.
struct failing_row {
  struct props {
    callback<void()> on_go;
    journal *log = nullptr;
  };

  element view(props const &given) {
    return button("go") | on_click([&given]() -> task<> {
             try {
               co_await given.on_go();
             } catch (std::runtime_error const &failure) {
               given.log->lines.emplace_back(failure.what());
             }
           });
  }
};

struct failing_page {
  state<bool> shows{true};
  journal log;

  element view() {
    return div({when(*shows, [this] {
      return component<failing_row>({.on_go = []() -> task<> {
                                       co_await quoll::async::sleep_for(1min);
                                       throw std::runtime_error{"refused"};
                                     },
                                     .log = &log});
    })});
  }
};

// Hands out a callback of its own, made in its view, that waits for an hour.
struct exposing {
  struct props {
    callback<void()> *out = nullptr;
  };

  element view(props const &given) {
    *given.out = []() -> task<> { co_await quoll::async::sleep_for(1h); };
    return p("exposing");
  }
};

struct exposing_page {
  state<bool> shows{true};
  callback<void()> *out = nullptr;

  element view() {
    return div({when(*shows, [this] { return component<exposing>({.out = out}); })});
  }
};

// Hands out a callback of the app's own component.
struct own_page {
  callback<void()> *out = nullptr;
  bool *ran = nullptr;

  element view() {
    *out = [this] { *ran = true; };
    return p("own");
  }
};

// Keeps a callback of its own view, and says when it is destroyed.
struct self_keeping {
  struct props {
    journal *log = nullptr;
  };

  callback<void()> mine;
  std::shared_ptr<void> gone;
  bool armed = false;

  element view(props const &given) {
    mine = [] {};

    if (!armed) {
      armed = true;
      gone = std::shared_ptr<void>{nullptr,
                                   [lines = &given.log->lines](void * /*nothing*/) { lines->emplace_back("gone"); }};
    }

    return p("keeping");
  }
};

struct self_keeping_page {
  state<bool> shows{true};
  journal log;

  element view() {
    return div({when(*shows, [this] { return component<self_keeping>({.log = &log}); })});
  }
};

static_assert(std::equality_comparable<callback<void(int)>>);

} // namespace

TEST_CASE("callback: a call runs in the task of the component that wrote it, which goes on when the caller goes",
          "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<saving_page> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The row goes while the page saves: it stops waiting, the save goes on.
    instance.component().shows = false;
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"saved a"});
}

TEST_CASE("callback: the caller waits for the end of the call", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<saving_page> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"saved a", "the row saw it saved"});
}

TEST_CASE("callback: the caller gets what the call gives", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<asking_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().log.lines == std::vector<std::string>{"got 4"});
}

TEST_CASE("callback: what a call throws reaches its caller, or on_error once the caller has gone", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  int failures = 0;
  test_app<failing_page> instance{time.get_executor(), shown, {.on_error = [&](std::exception_ptr) { ++failures; }}};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    // Seen by the row.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2min);

    // The row goes first: the page's on_error hears of it.
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    instance.component().shows = false;
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"refused"});
  REQUIRE(failures == 1);
}

TEST_CASE("callback: a call stopped as its component goes ends its caller on the stop", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<exposing_page> instance{time.get_executor(), shown};
  callback<void()> exposed;
  std::optional<std::monostate> ended{std::monostate{}};

  instance.component().out = &exposed;

  auto const call = [&]() -> task<> { ended = co_await quoll::async::stopped_as_optional(exposed()); };

  auto const leave = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await settle();
  };

  auto const act = [&]() -> task<> { co_await quoll::async::when_all(call(), leave()); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(ended);
  REQUIRE(time.now() < 1h);
}

TEST_CASE("callback: once the app has stopped, or is gone, a call ends on a stop and runs nothing", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  callback<void()> exposed;
  bool ran = false;

  auto const call = [&]() -> task<bool> {
    auto const ended = co_await quoll::async::stopped_as_optional(exposed());

    co_return ended.has_value();
  };

  {
    test_app<own_page> instance{time.get_executor(), shown};

    instance.component().out = &exposed;
    instance.component().ran = &ran;

    REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
    REQUIRE(quoll::test::run(time, call()) == false);
  }

  REQUIRE(quoll::test::run(time, call()) == false);
  REQUIRE_FALSE(ran);
}

TEST_CASE("callback: one made outside a view runs in the caller's task", "[ui][callback]") {
  callback<int(int)> const next = [](int value) { return value + 1; };
  callback<void(std::string const &)> const kept = [](std::string const &) -> task<> { co_return; };

  auto const call = [&]() -> task<int> {
    co_await kept("copied into the task");
    co_return co_await next(1);
  };

  REQUIRE(quoll::test::run(call()) == 2);
  REQUIRE(next == callback<int(int)>{});
  REQUIRE_FALSE(callback<void()>{});
}

TEST_CASE("callback: one kept by the component that made it does not keep that component", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<self_keeping_page> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await settle();
    REQUIRE(log == std::vector<std::string>{"gone"});
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
}
