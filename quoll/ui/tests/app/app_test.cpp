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

#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct counter_data {
  state<int> count;
  state<int> other;
};

constexpr component<> counter = [] {
  auto &self = use_context<counter_data>();

  return div({
             text("count: " + std::to_string(*self->count)),
             button("Add") | on_click([&self] { self->count = *self->count + 1; }),
         }) |
         tw("flex");
};

constexpr component<> no_state = [] { return p("static"); };

struct form_page_data {
  state<std::string> draft;
};

constexpr component<> form_page = [] {
  auto &self = use_context<form_page_data>();

  return input() | bind(self->draft) | attribute("placeholder", "note");
};

// Handlers borrowing what the event gave them, and reading it once they have waited.
struct borrowing_handlers_data {
  std::string viewed;
  std::string referred;
};

constexpr component<> borrowing_handlers = [] {
  auto &self = use_context<borrowing_handlers_data>();

  return input() |
         on("input",
            [&self](std::string_view value) -> task<> {
              co_await quoll::async::sleep_for(1s);
              self->viewed = value;
            }) |
         on("change", [&self](std::string const &value) -> task<> {
           co_await quoll::async::sleep_for(1s);
           self->referred = value;
         });
};

// A handler whose task returns something.
struct returning_handler_data {
  bool ran = false;
};

constexpr component<> returning_handler = [] {
  auto &self = use_context<returning_handler_data>();

  return button("Run") | on_click([&self]() -> task<int> {
           co_await quoll::async::sleep_for(1s);
           self->ran = true;
           co_return 1;
         });
};

// Handlers added directly to the element, without on().
struct direct_handlers_data {
  state<int> renders;
  std::shared_ptr<int> value = std::make_shared<int>(7);
  int seen = 0;
};

constexpr component<> direct_handlers = [] {
  auto &self = use_context<direct_handlers_data>();

  return button(std::to_string(*self->renders)) | decorator([&self](element &target) {
           target.events.push_back({.name = "click", .handler = [&self, held = self->value](std::string) -> task<> {
                                      co_await quoll::async::sleep_for(1h);
                                      self->seen = *held;
                                    }});
           target.events.push_back({.name = "focus", .handler = [](std::string) -> task<> {
                                      throw std::runtime_error{"before any task"};
                                    }});
         });
};

// An attribute set twice when a state says so.
struct overriding_attribute_data {
  state<bool> overridden;
};

constexpr component<> overriding_attribute = [] {
  auto &self = use_context<overriding_attribute_data>();

  auto field = input() | attribute("placeholder", "default");

  return *self->overridden ? std::move(field) | attribute("placeholder", "custom") : std::move(field);
};

// Two functions on one event of a node, the second depending on a state.
struct two_on_one_event_data {
  state<std::string> draft;
  state<bool> watched{true};
  int seen = 0;
};

constexpr component<> two_on_one_event = [] {
  auto &self = use_context<two_on_one_event_data>();

  auto field = input() | bind(self->draft);

  return *self->watched ? std::move(field) | on_input([&self](std::string const &) { ++self->seen; })
                        : std::move(field);
};

// A handler stopping the app it runs in.
struct stopping_handler_data {
  std::function<task<>()> *stop = nullptr;
};

constexpr component<> stopping_handler = [] {
  auto &self = use_context<stopping_handler_data>();

  return button("Stop") | on_click([&self]() -> task<> { co_await (*self->stop)(); });
};

// A first view asking for another render, then failing.
struct changing_then_failing_data {
  state<int> count;
  int views = 0;
};

constexpr component<> changing_then_failing = [] -> element {
  auto &self = use_context<changing_then_failing_data>();

  ++self->views;
  self->count = 1;
  throw std::runtime_error{"not yet"};
};

// A handler holding what reads the component when the handler goes.
struct handler_with_guard_data {
  struct guard {
    int *read = nullptr;
    int const *from = nullptr;

    guard() = default;

    guard(int *into, int const *source) : read(into), from(source) {}

    guard(guard const &) = default;
    guard &operator=(guard const &) = default;
    guard(guard &&) = default;
    guard &operator=(guard &&) = default;

    ~guard() {
      if (read != nullptr) {
        *read = *from;
      }
    }
  };

  // What its destructor changes shows whether the guard read it before or after.
  struct marker {
    int value = 7;

    marker() = default;
    marker(marker const &) = default;
    marker &operator=(marker const &) = default;
    marker(marker &&) = default;
    marker &operator=(marker &&) = default;

    ~marker() { value = 0; }
  };

  marker alive;
  int *read = nullptr;
};

constexpr component<> handler_with_guard = [] {
  auto &self = use_context<handler_with_guard_data>();

  return button("Go") |
         on_click([held = std::make_shared<handler_with_guard_data::guard>(self->read, &self->alive.value)] {});
};

// An input bound or not, depending on a state.
struct optional_binding_data {
  state<bool> bound{true};
  state<std::string> draft;
};

constexpr component<> optional_binding = [] {
  auto &self = use_context<optional_binding_data>();

  auto field = input();

  return *self->bound ? std::move(field) | bind(self->draft) : std::move(field);
};

struct notes_page_data {
  state<std::vector<std::string>> notes;
  state<std::string> status{std::string{"loading"}};
};

constexpr component<> notes_page = [] {
  auto &self = use_context<notes_page_data>();

  use_effect([&self]() -> task<> {
    co_await quoll::async::sleep_for(1s);
    self->notes = std::vector<std::string>{"a", "b"};
    self->status = std::string{"ready"};
  });

  return div({
      h1(*self->status),
      ul({list(self->notes, [](std::string const &note) { return li(note) | key(note); })}),
  });
};

struct keyed_page_data {
  state<std::vector<int>> order{std::vector<int>{1, 2, 3}};
};

constexpr component<> keyed_page = [] {
  auto &self = use_context<keyed_page_data>();

  return ul({list(self->order, [](int item) { return li(std::to_string(item)) | key(item); })});
};

struct changing_tag_data {
  state<bool> flag;
};

constexpr component<> changing_tag = [] {
  auto &self = use_context<changing_tag_data>();

  return *self->flag ? span("text") : div({p("text")});
};

struct changing_details_data {
  state<bool> flag;
};

constexpr component<> changing_details = [] {
  auto &self = use_context<changing_details_data>();

  auto shown = div() | tw_if(*self->flag, "on");

  if (*self->flag) {
    return std::move(shown) | attribute("id", "x") | on_click([] {});
  }

  return std::move(shown) | attribute("title", "y");
};

// Says, as it is destroyed, whether what `held` points to is still what it was: a frame unwound on a stop destroys it.
class held_until_the_end {
public:
  held_until_the_end(bool &kept, std::shared_ptr<int> const &held) noexcept : kept_(&kept), held_(&held) {}

  held_until_the_end(held_until_the_end const &) = delete;
  held_until_the_end(held_until_the_end &&) = delete;
  held_until_the_end &operator=(held_until_the_end const &) = delete;
  held_until_the_end &operator=(held_until_the_end &&) = delete;

  ~held_until_the_end() { *kept_ = **held_ == 7; }

private:
  bool *kept_;
  std::shared_ptr<int> const *held_;
};

struct slow_handler_data {
  state<int> done;
  bool *stopped = nullptr;
  std::shared_ptr<int> *watched = nullptr;
};

constexpr component<> slow_handler = [] {
  auto &self = use_context<slow_handler_data>();

  return button(std::to_string(*self->done)) | on_click([&self, held = *self->watched]() -> task<> {
           // The lambda, and what it captures, is alive until the stop has unwound this frame.
           held_until_the_end const told{*self->stopped, held};

           co_await quoll::async::sleep_for(1h);
           self->done = 1;
         });
};

// The trap #151 closes, bounded so as to end the test: a handler catching all around a sleep, and sleeping again.
struct handler_catching_all_data {
  int *caught = nullptr;
  int *turns = nullptr;
};

constexpr component<> handler_catching_all = [] {
  auto &self = use_context<handler_catching_all_data>();

  return button("Retry") | on_click([&self]() -> task<> {
           while (*self->turns < 1000) {
             ++*self->turns;

             try {
               co_await quoll::async::sleep_for(1h);
             } catch (std::exception const &) {
               ++*self->caught;
             }
           }
         });
};

constexpr component<> handler_ending_on_a_stop = [] {
  return button("Stop") | on_click([]() -> task<> {
           co_await quoll::async::sleep_for(1s);
           co_await quoll::async::detail::end_stopped{};
         });
};

constexpr component<> failing_handler = [] {
  return button("Fail") | on_click([]() -> task<> {
           co_await quoll::async::sleep_for(1s);
           throw std::runtime_error{"boom"};
         });
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

using test_app = app<memory_renderer>;

} // namespace

TEST_CASE("ui::renderer_ref forwards to its renderer, and gives it back", "[ui]") {
  memory_renderer shown;
  renderer_ref const erased{shown};

  erased.create_element(1, "p");
  erased.insert(no_node, 1, no_node);

  REQUIRE(shown.html() == "<p></p>");
  REQUIRE(erased.target<memory_renderer>() == &shown);
  REQUIRE(erased.target<renderer_ref>() == nullptr);
}

TEST_CASE("ui::app shows its component", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  REQUIRE(quoll::test::run(time, session(instance, []() -> task<> { co_await settle(); })) == 0);
  REQUIRE(shown.html() == R"(<div class="flex">count: 0<button>Add</button></div>)");
}

TEST_CASE("ui::app takes a component without any state", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, no_state};

  REQUIRE(quoll::test::run(time, session(instance, []() -> task<> { co_await settle(); })) == 0);
  REQUIRE(shown.html() == "<p>static</p>");
}

TEST_CASE("ui::app: an event changes a state, and only what differs reaches the renderer", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  auto const act = [&]() -> task<> {
    shown.reset_operations();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == R"(<div class="flex">count: 1<button>Add</button></div>)");
  REQUIRE(shown.operations() == 1);
}

TEST_CASE("ui::app: several changes show once", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};
  int operations_of_first_view = 0;

  auto const act = [&]() -> task<> {
    operations_of_first_view = shown.operations();
    shown.reset_operations();

    instance.context<counter_data>().count = 5;
    instance.context<counter_data>().count = 6;
    instance.context<counter_data>().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(operations_of_first_view > 1);
  REQUIRE(shown.html() == R"(<div class="flex">count: 6<button>Add</button></div>)");
  REQUIRE(shown.operations() == 1);
}

TEST_CASE("ui::app: a view that does not change costs no operation", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  auto const act = [&]() -> task<> {
    shown.reset_operations();
    instance.context<counter_data>().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.operations() == 0);
}

TEST_CASE("ui::app: an effect on nothing runs once its view first shows, and shows what it loads", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, notes_page, {.contexts = {provide<notes_page_data>()}}};
  std::string first;

  auto const act = [&]() -> task<> {
    first = shown.html();
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(first == "<div><h1>loading</h1><ul></ul></div>");
  REQUIRE(shown.html() == "<div><h1>ready</h1><ul><li>a</li><li>b</li></ul></div>");
}

TEST_CASE("ui::app: bind() keeps an input and a state the same", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, form_page, {.contexts = {provide<form_page_data>()}}};
  std::string first;
  std::string typed;

  auto const act = [&]() -> task<> {
    first = shown.html();

    instance.dispatch(shown.find("input"), "input", "typed");
    co_await settle();
    typed = shown.html();

    instance.context<form_page_data>().draft = std::string{"reset"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(first == R"(<input placeholder="note" value=""></input>)");
  REQUIRE(typed == R"(<input placeholder="note" value="typed"></input>)");
  REQUIRE(shown.html() == R"(<input placeholder="note" value="reset"></input>)");
}

TEST_CASE("ui::app: a handler can borrow the value of its event until its task ends", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, borrowing_handlers, {.contexts = {provide<borrowing_handlers_data>()}}};
  std::string const typed = "long enough not to fit in the string itself";

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("input"), "input", typed);
    instance.dispatch(shown.find("input"), "change", typed);
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<borrowing_handlers_data>().viewed == typed);
  REQUIRE(instance.context<borrowing_handlers_data>().referred == typed);
}

TEST_CASE("ui::app: a handler returning a task of something runs too", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, returning_handler, {.contexts = {provide<returning_handler_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<returning_handler_data>().ran);
}

TEST_CASE("ui::app: a handler added without on() stays alive until its task ends", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, direct_handlers, {.contexts = {provide<direct_handlers_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // Another view replaces the element, and the handler that was on it.
    instance.context<direct_handlers_data>().renders = 1;
    co_await settle();
    co_await quoll::async::sleep_for(2h);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<direct_handlers_data>().seen == 7);
}

TEST_CASE("ui::app: what a handler throws before its task goes to on_error", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  auto const told = [&](std::exception_ptr error) { failure = std::move(error); };
  test_app instance{
      time.get_executor(), shown, direct_handlers, {.on_error = told, .contexts = {provide<direct_handlers_data>()}}
  };

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "focus");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
}

TEST_CASE("ui::app: an event not started when stop() comes does not run", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  auto const click_then_stop = [&]() -> task<int> {
    instance.start();
    instance.dispatch(shown.find("button"), "click");
    co_await instance.stop();
    co_return *instance.context<counter_data>().count;
  };

  REQUIRE(quoll::test::run(time, click_then_stop()) == 0);
}

TEST_CASE("ui::app: keyed children keep their nodes when they move", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app instance{time.get_executor(), shown, keyed_page, {.contexts = {provide<keyed_page_data>()}}};
  std::vector<std::string> pages;
  std::vector<int> operations;
  quoll::ui::node_id two_before{};
  quoll::ui::node_id two_after{};

  auto const change_to = [&](std::vector<int> order) -> task<> {
    shown.reset_operations();
    instance.context<keyed_page_data>().order = std::move(order);
    co_await settle();
    pages.push_back(shown.html());
    operations.push_back(shown.operations());
  };

  auto const act = [&]() -> task<> {
    two_before = shown.find_text("2");

    co_await change_to({3, 1, 2});
    co_await change_to({2, 3});
    co_await change_to({2, 3, 4});

    two_after = shown.find_text("2");

    co_await change_to({});
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      pages ==
      std::vector<std::string>{
          "<ul><li>3</li><li>1</li><li>2</li></ul>", "<ul><li>2</li><li>3</li></ul>",
          "<ul><li>2</li><li>3</li><li>4</li></ul>", "<ul></ul>"
      }
  );
  REQUIRE(operations.at(0) == 1); // Only 3 moves.
  REQUIRE(two_before == two_after);
}

TEST_CASE("ui::app: an attribute set twice takes its last value, and loses it with the second", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{
      time.get_executor(), shown, overriding_attribute, {.contexts = {provide<overriding_attribute_data>()}}
  };
  std::string overridden;

  auto const act = [&]() -> task<> {
    instance.context<overriding_attribute_data>().overridden = true;
    co_await settle();
    overridden = shown.html();

    instance.context<overriding_attribute_data>().overridden = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(overridden == R"(<input placeholder="custom"></input>)");
  REQUIRE(shown.html() == R"(<input placeholder="default"></input>)");
}

TEST_CASE("ui::app: two functions on one event listen once and each run once", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, two_on_one_event, {.contexts = {provide<two_on_one_event_data>()}}};
  std::vector<std::string> first;

  auto const act = [&]() -> task<> {
    first = shown.events(shown.find("input"));

    instance.dispatch(shown.find("input"), "input", "typed");
    co_await settle();

    instance.context<two_on_one_event_data>().watched = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(first == std::vector<std::string>{"input"});
  REQUIRE(instance.context<two_on_one_event_data>().seen == 1);
  REQUIRE(*instance.context<two_on_one_event_data>().draft == "typed");
  REQUIRE(shown.events(shown.find("input")) == std::vector<std::string>{"input"});
}

TEST_CASE("ui::app: stop() from one of its handlers fails rather than wait for itself", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  auto const told = [&](std::exception_ptr error) { failure = std::move(error); };
  test_app instance{
      time.get_executor(), shown, stopping_handler, {.on_error = told, .contexts = {provide<stopping_handler_data>()}}
  };
  std::function<task<>()> stop = [&] { return instance.stop(); };

  instance.context<stopping_handler_data>().stop = &stop;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
}

TEST_CASE("ui::app: a start that fails stops the app, and what it left pending shows nothing", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{
      time.get_executor(), shown, changing_then_failing, {.contexts = {provide<changing_then_failing_data>()}}
  };
  bool failed = false;

  auto const start_then_stop = [&]() -> task<int> {
    try {
      instance.start();
    } catch (std::runtime_error const &) {
      failed = true;
    }

    co_await settle();
    co_await instance.stop();
    co_return instance.context<changing_then_failing_data>().views;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 1);
  REQUIRE(failed);
}

TEST_CASE("ui::app: the handlers of the view go before the component they may read", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  int read = 0;

  {
    test_app instance{
        time.get_executor(), shown, handler_with_guard, {.contexts = {provide<handler_with_guard_data>()}}
    };

    instance.context<handler_with_guard_data>().read = &read;
    REQUIRE(quoll::test::run(time, session(instance, []() -> task<> { co_await settle(); })) == 0);
  }

  REQUIRE(read == 7);
}

TEST_CASE("ui::app: another tag makes another node", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, changing_tag, {.contexts = {provide<changing_tag_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<changing_tag_data>().flag = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<span>text</span>");
}

TEST_CASE("ui::app: classes, attributes and events follow the view", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, changing_details, {.contexts = {provide<changing_details_data>()}}};
  std::string first;
  std::string changed;
  std::vector<std::string> first_events;
  std::vector<std::string> changed_events;

  auto const act = [&]() -> task<> {
    first = shown.html();
    first_events = shown.events(shown.find("div"));

    instance.context<changing_details_data>().flag = true;
    co_await settle();
    changed = shown.html();
    changed_events = shown.events(shown.find("div"));

    instance.context<changing_details_data>().flag = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(first == R"(<div title="y"></div>)");
  REQUIRE(first_events.empty());
  REQUIRE(changed == R"(<div class="on" id="x"></div>)");
  REQUIRE(changed_events == std::vector<std::string>{"click"});
  REQUIRE(shown.html() == first);
  REQUIRE(shown.events(shown.find("div")).empty());
}

TEST_CASE("ui::app: stop() stops a running handler, kept alive until it ends", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, slow_handler, {.contexts = {provide<slow_handler_data>()}}};
  bool stopped = false;
  auto watched = std::make_shared<int>(7);

  instance.context<slow_handler_data>().stopped = &stopped;
  instance.context<slow_handler_data>().watched = &watched;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // Another view replaces the element, and the handler that was on it.
    watched = std::make_shared<int>(0);
    instance.context<slow_handler_data>().done = 2;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(stopped);
  REQUIRE(time.now() < 1h);
}

TEST_CASE("ui::app: what a handler throws goes to on_error", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  auto const told = [&](std::exception_ptr error) { failure = std::move(error); };
  test_app instance{time.get_executor(), shown, failing_handler, {.on_error = told}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
}

TEST_CASE("ui::app: a handler ending on a stop is no failure: on_error does not hear of it", "[ui][stopped]") {
  virtual_time time;
  memory_renderer const shown;
  auto told = 0;
  test_app instance{time.get_executor(), shown, handler_ending_on_a_stop, {.on_error = [&](std::exception_ptr) {
                      ++told;
                    }}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2s);
  };

  // The app stops once its tasks have ended: the stopped one counts as ended.
  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(told == 0);
}

TEST_CASE(
    "ui::app: a handler catching all around a sleep does not see the app's stop, which ends it", "[ui][stopped]"
) {
  virtual_time time;
  memory_renderer const shown;
  auto caught = 0;
  auto turns = 0;
  test_app instance{
      time.get_executor(), shown, handler_catching_all, {.contexts = {provide<handler_catching_all_data>()}}
  };

  instance.context<handler_catching_all_data>().caught = &caught;
  instance.context<handler_catching_all_data>().turns = &turns;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(1s);
  };

  // stop() ends once the handler has: through once, its catch unseen.
  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(turns == 1);
  REQUIRE(caught == 0);
}

TEST_CASE("ui::app: an event for a node that is gone is ignored", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(9999, "click");
    instance.dispatch(shown.find("div"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(*instance.context<counter_data>().count == 0);
}

TEST_CASE("ui::app: an input no longer bound holds nothing", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, optional_binding, {.contexts = {provide<optional_binding_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<optional_binding_data>().draft = std::string{"kept"};
    co_await settle();

    instance.context<optional_binding_data>().bound = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == R"(<input value=""></input>)");
}

TEST_CASE("ui::app: nothing runs after stop()", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, counter, {.contexts = {provide<counter_data>()}}};

  auto const stop_then_click = [&]() -> task<int> {
    instance.start();

    auto const add = shown.find("button");

    co_await instance.stop();

    // Nothing waits for these any more: they must start nothing that could outlive the app.
    instance.dispatch(add, "click");
    instance.start();
    co_await settle();
    co_return *instance.context<counter_data>().count;
  };

  REQUIRE(quoll::test::run(time, stop_then_click()) == 0);
}

TEST_CASE("ui::app: a view that could not be scheduled is tried again at the next change", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  bool fail = false;
  test_app instance{
      failing_executor{time.get_executor(), &fail}, shown, counter, {.contexts = {provide<counter_data>()}}
  };
  bool thrown = false;

  auto const act = [&]() -> task<> {
    fail = true;

    try {
      instance.context<counter_data>().count = 1;
    } catch (std::runtime_error const &) {
      thrown = true;
    }

    fail = false;
    instance.context<counter_data>().count = 2;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(thrown);
  REQUIRE(shown.html() == R"(<div class="flex">count: 2<button>Add</button></div>)");
}

namespace {

// A button disabled while it waits.
struct waiting_button_data {
  state<bool> busy;
};

constexpr component<> waiting_button = [] {
  auto &self = use_context<waiting_button_data>();

  return button("Save") | disabled(*self->busy);
};

} // namespace

TEST_CASE("ui::app: disabled() sets the attribute while its condition holds, and takes it away after", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, waiting_button, {.contexts = {provide<waiting_button_data>()}}};
  std::string first;
  std::string waiting;

  auto const act = [&]() -> task<> {
    first = shown.html();

    instance.context<waiting_button_data>().busy = true;
    co_await settle();
    waiting = shown.html();

    instance.context<waiting_button_data>().busy = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first == "<button>Save</button>");
  CHECK(waiting == R"(<button disabled="">Save</button>)");
  CHECK(shown.html() == first);
}

namespace {

// The base decorators, set while their conditions hold, and the events they name.
struct decorated_form_data {
  state<bool> locked;
  state<std::vector<std::string>> heard;
};

constexpr component<> decorated_form = [] {
  auto &self = use_context<decorated_form_data>();

  auto const told = [&self](std::string what) {
    return
        [&self, what](std::string given) { self->heard.update([&](auto &all) { all.push_back(what + " " + given); }); };
  };

  return form({
             input() | placeholder("A note") | value("tea") | required(true) | readonly(*self->locked) |
                 aria("label", "note") | on_change(told("change")) | on_key_down(told("key")),
             p("hint") | role("note") | hidden(!*self->locked),
         }) |
         on_submit(told("submit"));
};

} // namespace

TEST_CASE("ui::app: the base decorators set their attributes, and their events reach their functions", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, decorated_form, {.contexts = {provide<decorated_form_data>()}}};
  std::string first;
  std::string locked;

  auto const act = [&]() -> task<> {
    first = shown.html();

    instance.dispatch(shown.find("input"), "change", "green tea");
    instance.dispatch(shown.find("input"), "keydown", "Enter");
    instance.dispatch(shown.find("form"), "submit");
    co_await settle();

    instance.context<decorated_form_data>().locked = true;
    co_await settle();
    locked = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(
      first == R"(<form><input aria-label="note" placeholder="A note" required="" value="tea"></input>)"
               R"(<p hidden="" role="note">hint</p></form>)"
  );
  CHECK(
      locked == R"(<form><input aria-label="note" placeholder="A note" readonly="" required="" value="tea"></input>)"
                R"(<p role="note">hint</p></form>)"
  );
  CHECK(
      *instance.context<decorated_form_data>().heard ==
      std::vector<std::string>{"change green tea", "key Enter", "submit "}
  );
}

namespace {

// A box bound to a state, and another checked from it, given as a value.
struct checked_boxes_data {
  state<bool> done;
};

constexpr component<> checked_boxes = [] {
  auto &self = use_context<checked_boxes_data>();

  return div({input() | bind(self->done), input() | attribute("type", "checkbox") | checked(!*self->done)});
};

} // namespace

TEST_CASE("ui::app: bind() keeps a checkbox and a bool the same, checked() shows one given", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, checked_boxes, {.contexts = {provide<checked_boxes_data>()}}};
  std::string first;
  std::string clicked;

  auto const act = [&]() -> task<> {
    first = shown.html();

    instance.dispatch(shown.find("input"), "change", "true");
    co_await settle();
    clicked = shown.html();

    instance.dispatch(shown.find("input"), "change", "false");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first == R"(<div><input type="checkbox"></input><input checked="" type="checkbox"></input></div>)");
  CHECK(clicked == R"(<div><input checked="" type="checkbox"></input><input type="checkbox"></input></div>)");
  CHECK_FALSE(*instance.context<checked_boxes_data>().done);
  CHECK(shown.html() == first);
}

TEST_CASE("to_html: a box checked has the attribute a page starts its property with", "[ui][html]") {
  CHECK(to_html(input() | attribute("type", "checkbox") | checked(true)) == R"(<input checked="" type="checkbox"/>)");
  CHECK(to_html(input() | attribute("type", "checkbox") | checked(false)) == R"(<input type="checkbox"/>)");
}

namespace {

// What a renderer is asked, in order: where a select's value comes among its options.
struct ordered_renderer {
  std::vector<std::string> *asked;

  void create_element(node_id, std::string_view tag) { asked->push_back("create " + std::string{tag}); }

  void create_text(node_id, std::string_view) {}

  void set_text(node_id, std::string_view) {}

  void set_class(node_id, std::string_view) {}

  void set_attribute(node_id, std::string_view, std::string_view) {}

  void remove_attribute(node_id, std::string_view) {}

  void set_value(node_id, std::string_view value) { asked->push_back("value " + std::string{value}); }

  void set_checked(node_id, bool) {}

  void listen(node_id, std::string_view) {}

  void unlisten(node_id, std::string_view) {}

  void insert(node_id, node_id, node_id) { asked->push_back("insert"); }

  void remove(node_id) {}
};

static_assert(Renderer<ordered_renderer>);

// A select of the options its state holds, the second chosen.
struct choosing_select_data {
  state<std::vector<std::string>> options{std::vector<std::string>{"a"}};
  state<int> other;
};

constexpr component<> choosing_select = [] {
  auto &self = use_context<choosing_select_data>();

  auto const option = [](std::string const &name) { return node("option", {text(name)}) | key(name); };

  return div({node("select", {list(self->options, option)}) | value("b"), p(std::to_string(*self->other))});
};

} // namespace

TEST_CASE("ui::app: a select is given its value once its options are placed, and again as they change", "[ui]") {
  virtual_time time;
  std::vector<std::string> asked;
  app instance{
      time.get_executor(), ordered_renderer{&asked}, choosing_select, {.contexts = {provide<choosing_select_data>()}}
  };
  std::vector<std::string> first;
  std::vector<std::string> changed;

  auto const act = [&]() -> task<> {
    first = asked;
    asked.clear();

    instance.context<choosing_select_data>().options.update([](auto &all) { all.push_back("b"); });
    co_await settle();
    changed = asked;
    asked.clear();

    // Its options the same: what the user chose stays.
    instance.context<choosing_select_data>().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // Its option made, its text in it and it in the select, then its value, then the select placed in the page.
  CHECK(
      std::ranges::search(
          first, std::vector<std::string>{"create select", "create option", "insert", "insert", "value b"}
      )
          .begin() != first.end()
  );
  CHECK(changed.back() == "value b");
  CHECK(std::ranges::find(asked, "value b") == asked.end());
}

namespace {

// A select of two options with no key, the second's text a state's, inside a group.
struct relabelled_select_data {
  state<std::string> second{std::string{"c"}};
  state<int> other;
};

constexpr component<> relabelled_select = [] {
  auto &self = use_context<relabelled_select_data>();

  auto const group = node("optgroup", {node("option", {text("a")}), node("option", {text(*self->second)})});

  return div({node("select", {group}) | value("b"), p(std::to_string(*self->other))});
};

} // namespace

TEST_CASE("ui::app: a select is given its value again as an option within it changes what it says", "[ui]") {
  virtual_time time;
  std::vector<std::string> asked;
  app instance{
      time.get_executor(),
      ordered_renderer{&asked},
      relabelled_select,
      {.contexts = {provide<relabelled_select_data>()}}
  };
  std::vector<std::string> relabelled;

  auto const act = [&]() -> task<> {
    asked.clear();

    instance.context<relabelled_select_data>().second = std::string{"b"};
    co_await settle();
    relabelled = asked;
    asked.clear();

    instance.context<relabelled_select_data>().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(std::ranges::find(relabelled, "value b") != relabelled.end());
  CHECK(std::ranges::find(asked, "value b") == asked.end());
}
