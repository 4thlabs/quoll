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
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
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
using quoll::test::virtual_time;

struct counter {
  state<int> count;
  state<int> other;

  element view() {
    return div({
               text("count: " + std::to_string(*count)),
               button("Add") | on_click([this] { count = *count + 1; }),
           }) |
           tw("flex");
  }
};

struct no_state {
  element view() { return p("static"); }
};

struct form_page {
  state<std::string> draft;

  element view() { return input() | bind(draft) | attribute("placeholder", "note"); }
};

// Handlers borrowing what the event gave them, and reading it once they have waited.
struct borrowing_handlers {
  std::string viewed;
  std::string referred;

  element view() {
    return input() |
           on("input",
              [this](std::string_view value) -> task<> {
                co_await quoll::async::sleep_for(1s);
                viewed = value;
              }) |
           on("change", [this](std::string const &value) -> task<> {
             co_await quoll::async::sleep_for(1s);
             referred = value;
           });
  }
};

// A handler whose task returns something.
struct returning_handler {
  bool ran = false;

  element view() {
    return button("Run") | on_click([this]() -> task<int> {
             co_await quoll::async::sleep_for(1s);
             ran = true;
             co_return 1;
           });
  }
};

// Handlers added directly to the element, without on().
struct direct_handlers {
  state<int> renders;
  std::shared_ptr<int> value = std::make_shared<int>(7);
  int seen = 0;

  element view() {
    return button(std::to_string(*renders)) | decorator([this](element &target) {
             target.events.push_back({"click", [this, held = value](std::string) -> task<> {
                                        co_await quoll::async::sleep_for(1h);
                                        seen = *held;
                                      }});
             target.events.push_back(
                 {"focus", [](std::string) -> task<> { throw std::runtime_error{"before any task"}; }});
           });
  }
};

// An input bound or not, depending on a state.
struct optional_binding {
  state<bool> bound{true};
  state<std::string> draft;

  element view() {
    auto field = input();

    return *bound ? std::move(field) | bind(draft) : std::move(field);
  }
};

struct notes_page {
  state<std::vector<std::string>> notes;
  state<std::string> status{std::string{"loading"}};

  task<> mounted() {
    co_await quoll::async::sleep_for(1s);
    notes = std::vector<std::string>{"a", "b"};
    status = std::string{"ready"};
  }

  element view() {
    return div({
        h1(*status),
        ul({list(notes, [](std::string const &note) { return li(note) | key(note); })}),
    });
  }
};

struct keyed_page {
  state<std::vector<int>> order{std::vector<int>{1, 2, 3}};

  element view() {
    return ul({list(order, [](int item) { return li(std::to_string(item)) | key(item); })});
  }
};

struct changing_tag {
  state<bool> flag;

  element view() { return *flag ? span("text") : div({p("text")}); }
};

struct changing_details {
  state<bool> flag;

  element view() {
    auto shown = div() | tw_if(*flag, "on");

    if (*flag) {
      return std::move(shown) | attribute("id", "x") | on_click([] {});
    }

    return std::move(shown) | attribute("title", "y");
  }
};

struct slow_handler {
  state<int> done;
  bool *stopped = nullptr;
  std::shared_ptr<int> *watched = nullptr;

  element view() {
    return button(std::to_string(*done)) | on_click([this, held = *watched]() -> task<> {
             try {
               co_await quoll::async::sleep_for(1h);
             } catch (std::system_error const &) {
               // The lambda, and what it captures, is alive until here.
               *stopped = *held == 7;
               throw;
             }

             done = 1;
           });
  }
};

struct failing_handler {
  element view() {
    return button("Fail") | on_click([]() -> task<> {
             co_await quoll::async::sleep_for(1s);
             throw std::runtime_error{"boom"};
           });
  }
};

// Starts `the_app`, runs `act`, then stops it: all of it within one run of the clock.
template <class App, class F>
task<int> session(App &the_app, F act) {
  the_app.start();
  co_await act();
  co_await the_app.stop();
  co_return 0;
}

template <class C>
using test_app = app<C, memory_renderer>;

// Lets what is pending run.
task<> settle() { co_await quoll::async::sleep_for(1ms); }

} // namespace

TEST_CASE("ui::app shows its component", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<counter> instance{time.get_executor(), shown};

  REQUIRE(quoll::test::run(time, session(instance, []() -> task<> { co_await settle(); })) == 0);
  REQUIRE(shown.html() == R"(<div class="flex">count: 0<button>Add</button></div>)");
}

TEST_CASE("ui::app takes a component without any state", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<no_state> instance{time.get_executor(), shown};

  REQUIRE(quoll::test::run(time, session(instance, []() -> task<> { co_await settle(); })) == 0);
  REQUIRE(shown.html() == "<p>static</p>");
}

TEST_CASE("ui::app: an event changes a state, and only what differs reaches the renderer", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app<counter> instance{time.get_executor(), shown};

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
  test_app<counter> instance{time.get_executor(), shown};
  int operations_of_first_view = 0;

  auto const act = [&]() -> task<> {
    operations_of_first_view = shown.operations();
    shown.reset_operations();

    instance.component().count = 5;
    instance.component().count = 6;
    instance.component().other = 1;
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
  test_app<counter> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    shown.reset_operations();
    instance.component().other = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.operations() == 0);
}

TEST_CASE("ui::app runs mounted(), and shows what it loads", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<notes_page> instance{time.get_executor(), shown};
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
  test_app<form_page> instance{time.get_executor(), shown};
  std::string first;
  std::string typed;

  auto const act = [&]() -> task<> {
    first = shown.html();

    instance.dispatch(shown.find("input"), "input", "typed");
    co_await settle();
    typed = shown.html();

    instance.component().draft = std::string{"reset"};
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
  test_app<borrowing_handlers> instance{time.get_executor(), shown};
  std::string const typed = "long enough not to fit in the string itself";

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("input"), "input", typed);
    instance.dispatch(shown.find("input"), "change", typed);
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().viewed == typed);
  REQUIRE(instance.component().referred == typed);
}

TEST_CASE("ui::app: a handler returning a task of something runs too", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<returning_handler> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().ran);
}

TEST_CASE("ui::app: a handler added without on() stays alive until its task ends", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<direct_handlers> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // Another view replaces the element, and the handler that was on it.
    instance.component().renders = 1;
    co_await settle();
    co_await quoll::async::sleep_for(2h);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().seen == 7);
}

TEST_CASE("ui::app: what a handler throws before its task goes to on_error", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app<direct_handlers> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};

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
  test_app<counter> instance{time.get_executor(), shown};

  auto const click_then_stop = [&]() -> task<int> {
    instance.start();
    instance.dispatch(shown.find("button"), "click");
    co_await instance.stop();
    co_return *instance.component().count;
  };

  REQUIRE(quoll::test::run(time, click_then_stop()) == 0);
}

TEST_CASE("ui::app: keyed children keep their nodes when they move", "[ui]") {
  virtual_time time;
  memory_renderer shown;
  test_app<keyed_page> instance{time.get_executor(), shown};
  std::vector<std::string> pages;
  std::vector<int> operations;
  quoll::ui::node_id two_before{};
  quoll::ui::node_id two_after{};

  auto const change_to = [&](std::vector<int> order) -> task<> {
    shown.reset_operations();
    instance.component().order = std::move(order);
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
  REQUIRE(pages == std::vector<std::string>{"<ul><li>3</li><li>1</li><li>2</li></ul>", "<ul><li>2</li><li>3</li></ul>",
                                            "<ul><li>2</li><li>3</li><li>4</li></ul>", "<ul></ul>"});
  REQUIRE(operations.at(0) == 1); // Only 3 moves.
  REQUIRE(two_before == two_after);
}

TEST_CASE("ui::app: another tag makes another node", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<changing_tag> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().flag = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<span>text</span>");
}

TEST_CASE("ui::app: classes, attributes and events follow the view", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<changing_details> instance{time.get_executor(), shown};
  std::string first;
  std::string changed;
  std::vector<std::string> first_events;
  std::vector<std::string> changed_events;

  auto const act = [&]() -> task<> {
    first = shown.html();
    first_events = shown.events(shown.find("div"));

    instance.component().flag = true;
    co_await settle();
    changed = shown.html();
    changed_events = shown.events(shown.find("div"));

    instance.component().flag = false;
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
  test_app<slow_handler> instance{time.get_executor(), shown};
  bool stopped = false;
  auto watched = std::make_shared<int>(7);

  instance.component().stopped = &stopped;
  instance.component().watched = &watched;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // Another view replaces the element, and the handler that was on it.
    watched = std::make_shared<int>(0);
    instance.component().done = 2;
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
  test_app<failing_handler> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(2s);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
}

TEST_CASE("ui::app: an event for a node that is gone is ignored", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<counter> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(9999, "click");
    instance.dispatch(shown.find("div"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(*instance.component().count == 0);
}

TEST_CASE("ui::app: an input no longer bound holds nothing", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<optional_binding> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().draft = std::string{"kept"};
    co_await settle();

    instance.component().bound = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == R"(<input value=""></input>)");
}

TEST_CASE("ui::app: nothing runs after stop()", "[ui]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<counter> instance{time.get_executor(), shown};

  auto const stop_then_click = [&]() -> task<int> {
    instance.start();

    auto const add = shown.find("button");

    co_await instance.stop();

    // Nothing waits for these any more: they must start nothing that could outlive the app.
    instance.dispatch(add, "click");
    instance.start();
    co_await settle();
    co_return *instance.component().count;
  };

  REQUIRE(quoll::test::run(time, stop_then_click()) == 0);
}
