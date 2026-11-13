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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <quoll/ui/detail/callback_traits.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// What a callback, an action or an effect refuses to copy: views, also inside the standard types that hold others.
static_assert(quoll::ui::detail::holds_view<std::optional<std::string_view>>);
static_assert(quoll::ui::detail::holds_view<std::pair<std::string_view const, int>>);
static_assert(quoll::ui::detail::holds_view<std::variant<int, std::tuple<std::span<int const>>>>);
static_assert(quoll::ui::detail::holds_view<std::expected<int, std::string_view>>);
static_assert(quoll::ui::detail::holds_view<std::optional<int> const &>);
static_assert(!quoll::ui::detail::holds_view<std::map<std::string, std::vector<std::string>>>);
static_assert(!quoll::ui::detail::holds_view<std::optional<int>> && !quoll::ui::detail::holds_view<void>);

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

struct journal {
  std::vector<std::string> lines;
};

// Writes `line` into `log` as it is destroyed: what holds it says, as it goes, that it has.
class telling {
public:
  telling(journal *log, std::string line) noexcept : log_(log), line_(std::move(line)) {}

  telling(telling const &) = delete;
  telling(telling &&) = delete;
  telling &operator=(telling const &) = delete;
  telling &operator=(telling &&) = delete;

  ~telling() { log_->lines.push_back(line_); }

private:
  journal *log_;
  std::string line_;
};

// A row asking its parent to save it, and saying once it has.
struct saving_row_props {
  std::string label;
  callback<void(std::string)> on_save;
  journal *log = nullptr;
};

constexpr component<saving_row_props> saving_row = [](saving_row_props const &given) {
  return button(given.label) | on_click([&given]() -> task<> {
           co_await given.on_save(given.label);
           given.log->lines.emplace_back("the row saw it saved");
         });
};

// Saves after a minute, as a server would answer.
struct saving_page_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> saving_page = [] {
  auto &self = use_context<saving_page_data>();

  auto const save = [&self](std::string const &label) -> task<> {
    co_await quoll::async::sleep_for(1min);
    self->log.lines.push_back("saved " + label);
  };

  return div({when(*self->shows, [&] { return saving_row({.label = "a", .on_save = save, .log = &self->log}); })});
};

// Passes the callback it is given on to a row of its own, as it is.
struct relay_props {
  callback<void(std::string)> on_save;
  journal *log = nullptr;
};

constexpr component<relay_props> relay = [](relay_props const &given) {
  return saving_row({.label = "relayed", .on_save = given.on_save, .log = given.log});
};

struct relaying_page_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> relaying_page = [] {
  auto &self = use_context<relaying_page_data>();

  auto const save = [&self](std::string label) -> task<> {
    co_await quoll::async::sleep_for(1min);
    self->log.lines.push_back("saved " + label);
  };

  return div({when(*self->shows, [&] { return relay({.on_save = save, .log = &self->log}); })});
};

// A row asking its parent for a value.
struct asking_row_props {
  callback<int(int)> twice;
  journal *log = nullptr;
};

constexpr component<asking_row_props> asking_row = [](asking_row_props const &given) {
  return button("ask") | on_click([&given]() -> task<> {
           auto const answer = co_await given.twice(2);

           given.log->lines.push_back("got " + std::to_string(answer));
         });
};

struct asking_page_data {
  journal log;
};

constexpr component<> asking_page = [] {
  auto &self = use_context<asking_page_data>();

  return div({asking_row({.twice = [](int value) { return value * 2; }, .log = &self->log})});
};

// A row whose call fails, and which sees it.
struct failing_row_props {
  callback<void()> on_go;
  journal *log = nullptr;
};

constexpr component<failing_row_props> failing_row = [](failing_row_props const &given) {
  return button("go") | on_click([&given]() -> task<> {
           try {
             co_await given.on_go();
           } catch (std::runtime_error const &failure) {
             given.log->lines.emplace_back(failure.what());
           }
         });
};

struct failing_page_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> failing_page = [] {
  auto &self = use_context<failing_page_data>();

  auto const go = []() -> task<> {
    co_await quoll::async::sleep_for(1min);
    throw std::runtime_error{"refused"};
  };

  return div({when(*self->shows, [&] { return failing_row({.on_go = go, .log = &self->log}); })});
};

// Hands out a callback of its own, made in its view, that says it ran, then waits for an hour.
struct exposing_props {
  callback<void()> *out = nullptr;
  bool *ran = nullptr;
};

constexpr component<exposing_props> exposing = [](exposing_props const &given) {
  *given.out = [ran = given.ran]() -> task<> {
    *ran = true;
    co_await quoll::async::sleep_for(1h);
  };
  return p("exposing");
};

struct exposing_page_data {
  state<bool> shows{true};
  callback<void()> *out = nullptr;
  bool ran = false;
};

constexpr component<> exposing_page = [] {
  auto &self = use_context<exposing_page_data>();

  return div({when(*self->shows, [&self] { return exposing({.out = self->out, .ran = &self->ran}); })});
};

// Hands out a callback of the app's root, which says it ran after a minute.
struct own_page_data {
  callback<void()> *out = nullptr;
  bool *ran = nullptr;
};

constexpr component<> own_page = [] {
  auto &self = use_context<own_page_data>();

  *self->out = [&self]() -> task<> {
    co_await quoll::async::sleep_for(1min);
    *self->ran = true;
  };
  return p("own");
};

// What a component holds as long as it is there, made in its first view: a callback, and what says when it goes.
struct holding {
  callback<void()> kept;
  bool ran = false;
  telling gone;

  explicit holding(journal *log) : gone(log, "gone") {}
};

// Made once, in the first view of the component calling it: setting it views that component once more.
[[nodiscard]] holding &use_holding(journal *log) {
  auto &held = use_state<std::shared_ptr<holding>>();

  if (!*held) {
    held = std::make_shared<holding>(log);
  }

  return **held;
}

// Keeps the callback a component of its view hands it, and says when it is destroyed.
struct keeping_parent_props {
  journal *log = nullptr;
};

constexpr component<keeping_parent_props> keeping_parent = [](keeping_parent_props const &given) {
  auto &held = use_holding(given.log);

  return exposing({.out = &held.kept, .ran = &held.ran});
};

struct keeping_parent_page_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> keeping_parent_page = [] {
  auto &self = use_context<keeping_parent_page_data>();

  return div({when(*self->shows, [&self] { return keeping_parent({.log = &self->log}); })});
};

// Keeps a callback of its own view, and says when it is destroyed.
constexpr component<keeping_parent_props> self_keeping = [](keeping_parent_props const &given) {
  auto &held = use_holding(given.log);

  held.kept = [] {};
  return p("keeping");
};

struct self_keeping_page_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> self_keeping_page = [] {
  auto &self = use_context<self_keeping_page_data>();

  return div({when(*self->shows, [&self] { return self_keeping({.log = &self->log}); })});
};

// Hands out, on its first view only, a callback reading its props by reference, then views again with others.
struct stale_exposing_props {
  std::string label;
  callback<void()> *out = nullptr;
  std::vector<std::string> *read = nullptr;
};

constexpr component<stale_exposing_props> stale_exposing = [](stale_exposing_props const &given) {
  auto &armed = use_state(false);

  if (!*armed) {
    armed = true;
    *given.out = [&given] { given.read->push_back(given.label); };
  }

  return p(given.label);
};

struct stale_page_data {
  state<std::string> label{std::string{"the first label, too long for a short string"}};
  callback<void()> *out = nullptr;
  std::vector<std::string> read;
};

constexpr component<> stale_page = [] {
  auto &self = use_context<stale_page_data>();

  return div({stale_exposing({.label = *self->label, .out = self->out, .read = &self->read})});
};

// Makes a callback in an effect, not its view: it knows no component, and runs in its caller's task.
struct mounting_data {
  callback<void()> *out = nullptr;
  bool ran = false;
};

constexpr component<> mounting = [] {
  auto &self = use_context<mounting_data>();

  use_effect([&self]() -> task<> {
    *self->out = [&self]() -> task<> {
      co_await quoll::async::sleep_for(1min);
      self->ran = true;
    };
    co_return;
  });

  return p("mounting");
};

static_assert(std::equality_comparable<callback<void(int)>>);
// Another signature's callback would be called back here, in the view that converts it: refused.
static_assert(!std::convertible_to<callback<void(long)>, callback<void(int)>>);

} // namespace

TEST_CASE(
    "callback: a call runs in the task of the component that wrote it, which goes on when the caller goes",
    "[ui][callback]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, saving_page, {.contexts = {provide<saving_page_data>()}}};
  auto const &log = instance.context<saving_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The row goes while the page saves: it stops waiting, the save goes on.
    instance.context<saving_page_data>().shows = false;
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"saved a"});
}

TEST_CASE("callback: passed on as it is, it still calls back the component that wrote it", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, relaying_page, {.contexts = {provide<relaying_page_data>()}}};
  auto const &log = instance.context<relaying_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The relay and its row go: the page's save goes on.
    instance.context<relaying_page_data>().shows = false;
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"saved relayed"});
}

TEST_CASE("callback: one made for to_html calls nothing once the page is written", "[ui][callback]") {
  callback<void()> exposed;
  bool ran = false;

  std::ignore = to_html(div({exposing({.out = &exposed, .ran = &ran})}));

  auto const call = [&]() -> task<bool> {
    auto const ended = co_await quoll::async::stopped_as_optional(exposed());

    co_return ended.has_value();
  };

  REQUIRE(quoll::test::run(call()) == false);
  REQUIRE_FALSE(ran);
}

TEST_CASE("callback: the caller waits for the end of the call", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, saving_page, {.contexts = {provide<saving_page_data>()}}};
  auto const &log = instance.context<saving_page_data>().log.lines;

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
  test_app instance{time.get_executor(), shown, asking_page, {.contexts = {provide<asking_page_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<asking_page_data>().log.lines == std::vector<std::string>{"got 4"});
}

TEST_CASE("callback: what a call throws reaches its caller, or on_error once the caller has gone", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  int failures = 0;
  test_app instance{
      time.get_executor(),
      shown,
      failing_page,
      {.on_error = [&](std::exception_ptr) { ++failures; }, .contexts = {provide<failing_page_data>()}}
  };
  auto const &log = instance.context<failing_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    // Seen by the row.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2min);

    // The row goes first: the page's on_error hears of it.
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    instance.context<failing_page_data>().shows = false;
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"refused"});
  REQUIRE(failures == 1);
}

TEST_CASE("callback: a call stopped as its component goes ends its caller on the stop", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, exposing_page, {.contexts = {provide<exposing_page_data>()}}};
  callback<void()> exposed;
  std::optional<std::monostate> ended{std::monostate{}};

  instance.context<exposing_page_data>().out = &exposed;

  auto const call = [&]() -> task<> { ended = co_await quoll::async::stopped_as_optional(exposed()); };

  auto const leave = [&]() -> task<> {
    co_await settle();
    instance.context<exposing_page_data>().shows = false;
    co_await settle();
  };

  auto const act = [&]() -> task<> { co_await quoll::async::when_all(call(), leave()); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(ended);
  REQUIRE(time.now() < 1h);
}

TEST_CASE("callback: a call once its component is gone, or going, ends on a stop and runs nothing", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, exposing_page, {.contexts = {provide<exposing_page_data>()}}};
  callback<void()> exposed;
  std::optional<std::monostate> ended{std::monostate{}};

  instance.context<exposing_page_data>().out = &exposed;

  std::optional<std::monostate> ended_later{std::monostate{}};

  auto const act = [&]() -> task<> {
    co_await settle();

    // Going: the render that unmounts it is posted before the call's work, which then finds it stopped.
    instance.context<exposing_page_data>().shows = false;
    ended = co_await quoll::async::stopped_as_optional(exposed());

    // Gone.
    co_await settle();
    ended_later = co_await quoll::async::stopped_as_optional(exposed());
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(ended);
  REQUIRE_FALSE(ended_later);
  REQUIRE_FALSE(instance.context<exposing_page_data>().ran);
}

TEST_CASE("callback: a caller stopped already starts nothing", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, exposing_page, {.contexts = {provide<exposing_page_data>()}}};
  callback<void()> exposed;
  std::stop_source stopped;

  instance.context<exposing_page_data>().out = &exposed;
  stopped.request_stop();

  auto const call = [&]() -> task<> { co_await exposed(); };

  auto const act = [&]() -> task<> {
    co_await settle();
    co_await quoll::async::co_spawn(call(), quoll::async::detached, stopped.get_token());
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(instance.context<exposing_page_data>().ran);
}

TEST_CASE(
    "callback: one of the view of the app's root runs as a task of the root, which a caller's stop leaves running",
    "[ui][callback]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, own_page, {.contexts = {provide<own_page_data>()}}};
  callback<void()> exposed;
  bool ran = false;
  std::stop_source caller;

  instance.context<own_page_data>().out = &exposed;
  instance.context<own_page_data>().ran = &ran;

  auto const call = [&]() -> task<> { co_await exposed(); };

  auto const act = [&]() -> task<> {
    co_await settle();
    co_await quoll::async::co_spawn(call(), quoll::async::detached, caller.get_token());
    co_await settle();

    // Run in the caller's task, the work would stop with it.
    caller.request_stop();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(ran);
}

TEST_CASE("callback: one kept by the parent of the component that made it keeps neither", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{
      time.get_executor(), shown, keeping_parent_page, {.contexts = {provide<keeping_parent_page_data>()}}
  };
  auto const &log = instance.context<keeping_parent_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<keeping_parent_page_data>().shows = false;
    co_await settle();
    REQUIRE(log == std::vector<std::string>{"gone"});
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
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
    test_app instance{time.get_executor(), shown, own_page, {.contexts = {provide<own_page_data>()}}};

    instance.context<own_page_data>().out = &exposed;
    instance.context<own_page_data>().ran = &ran;

    REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
    REQUIRE(quoll::test::run(time, call()) == false);
  }

  REQUIRE(quoll::test::run(time, call()) == false);
  REQUIRE_FALSE(ran);
}

TEST_CASE(
    "callback: one made outside a view runs in the caller's task, with copies of its arguments", "[ui][callback]"
) {
  std::vector<std::string> seen;
  callback<int(int)> const next = [](int value) { return value + 1; };
  callback<long(int)> const widened = [](int value) { return value * 2; };
  callback<void(std::string const &)> const kept = [&seen](std::string const &text) -> task<> {
    co_await quoll::async::yield();
    seen.push_back(text);
  };

  auto const call = [&]() -> task<int> {
    // The temporary is gone once the call suspends: the task holds a copy.
    auto pending = kept(std::string{"copied into the task, too long for a short string"});

    co_await std::move(pending);
    co_return static_cast<int>(co_await widened(co_await next(1)));
  };

  REQUIRE(quoll::test::run(call()) == 4);
  REQUIRE(seen == std::vector<std::string>{"copied into the task, too long for a short string"});
}

TEST_CASE("callback: those calling something compare equal, and an empty one throws", "[ui][callback]") {
  callback<int(int)> const next = [](int value) { return value + 1; };
  callback<int(int)> const other = [](int value) { return value - 1; };
  callback<int(int)> const empty;

  REQUIRE(next == other);
  REQUIRE_FALSE(next == empty);
  REQUIRE(empty == callback<int(int)>{});
  REQUIRE_FALSE(empty);
  REQUIRE_FALSE(callback<int(int)>{static_cast<int (*)(int)>(nullptr)});
  REQUIRE_FALSE(callback<int(int)>{std::function<int(int)>{}});
  REQUIRE_THROWS_AS(quoll::test::run(empty(1)), std::bad_function_call);
}

TEST_CASE("callback: one kept by the component that made it does not keep that component", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, self_keeping_page, {.contexts = {provide<self_keeping_page_data>()}}};
  auto const &log = instance.context<self_keeping_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<self_keeping_page_data>().shows = false;
    co_await settle();
    REQUIRE(log == std::vector<std::string>{"gone"});
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
}

TEST_CASE(
    "callback: a call once the view that made it has been replaced throws, as its given may be gone", "[ui][callback]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, stale_page, {.contexts = {provide<stale_page_data>()}}};
  callback<void()> exposed;
  std::optional<std::monostate> first;

  instance.context<stale_page_data>().out = &exposed;

  auto const act = [&]() -> task<> {
    co_await settle();
    first = co_await quoll::async::stopped_as_optional(exposed());

    instance.context<stale_page_data>().label = std::string{"the second label, too long for a short string"};
    co_await settle();

    auto thrown = false;

    try {
      co_await exposed();
    } catch (std::logic_error const &) {
      thrown = true;
    }

    REQUIRE(thrown);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(first);
  REQUIRE(
      instance.context<stale_page_data>().read ==
      std::vector<std::string>{"the first label, too long for a short string"}
  );
}

TEST_CASE("callback: a function taking its argument by reference refers to the task's own copy", "[ui][callback]") {
  std::vector<std::string> seen;
  callback<void(std::string)> const kept = [&seen](std::string const &text) -> task<> {
    co_await quoll::async::yield();
    seen.push_back(text);
  };

  auto const call = [&]() -> task<int> {
    co_await kept(std::string{"held by the task, too long for a short string"});
    co_return 0;
  };

  REQUIRE(quoll::test::run(call()) == 0);
  REQUIRE(seen == std::vector<std::string>{"held by the task, too long for a short string"});
}

TEST_CASE("callback: one made in an effect runs in its caller's task, and stops with it", "[ui][callback]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, mounting, {.contexts = {provide<mounting_data>()}}};
  callback<void()> made;
  std::stop_source caller;

  instance.context<mounting_data>().out = &made;

  auto const call = [&]() -> task<> { co_await made(); };

  auto const act = [&]() -> task<> {
    co_await settle();
    co_await quoll::async::co_spawn(call(), quoll::async::detached, caller.get_token());
    co_await settle();
    caller.request_stop();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_FALSE(instance.context<mounting_data>().ran);
}

TEST_CASE("callback: an argument that can only move moves into the function", "[ui][callback]") {
  callback<int(std::unique_ptr<int>)> const opened = [](std::unique_ptr<int> held) { return *held; };

  auto const call = [&]() -> task<int> { co_return co_await opened(std::make_unique<int>(7)); };

  REQUIRE(quoll::test::run(call()) == 7);
}
