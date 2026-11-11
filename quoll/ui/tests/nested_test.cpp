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
#include <quoll/async/work_item.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/node_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::virtual_time;

template <class C>
using test_app = app<C, memory_renderer>;

template <class App, class F>
task<int> session(App &the_app, F act) {
  the_app.start();
  co_await act();
  co_await the_app.stop();
  co_return 0;
}

task<> settle() { co_await quoll::async::sleep_for(1ms); }

// Says what comes and goes: its props tell where.
struct journal {
  std::vector<std::string> lines;
};

// A row with a count of its own, and the label its parent gives it.
struct row {
  struct props {
    std::string label;
    journal *log = nullptr;
  };

  state<int> clicks;
  journal *log = nullptr;
  std::string label;

  element view(props const &given) {
    if (given.label == "boom") {
      throw std::runtime_error{"no such row"};
    }

    log = given.log;
    label = given.label;
    return button(given.label + ":" + std::to_string(*clicks)) | on_click([this] { clicks = *clicks + 1; });
  }

  task<> mounted() {
    log->lines.push_back("mounted " + label);
    co_return;
  }

  void unmounted() { log->lines.push_back("unmounted " + label); }
};

// Rows by label, in the order it holds them, under a header that counts the views.
struct rows {
  state<std::vector<std::string>> labels{std::vector<std::string>{"a", "b"}};
  state<int> views;
  journal log;

  element view() {
    return div({
        p("views") | on_click([this] { views = *views + 1; }),
        ul({list(
            labels,
            [this](std::string const &label) { return component<row>({.label = label, .log = &log}) | key(label); })}),
    });
  }
};

// A row whose click waits for an hour, so that it is still running as the row goes.
struct slow_row {
  struct props {
    journal *log = nullptr;
  };

  journal *log = nullptr;

  element view(props const &given) {
    log = given.log;
    return button("slow") | on_click([this]() -> task<> {
             // Destroyed as the stop unwinds it: the row is still there then, kept by this task.
             struct told {
               journal *log;

               explicit told(journal *to) noexcept : log(to) {}
               told(told const &) = delete;
               told(told &&) = delete;
               told &operator=(told const &) = delete;
               told &operator=(told &&) = delete;

               ~told() { log->lines.emplace_back("handler ended"); }
             } const ending{log};

             co_await quoll::async::sleep_for(1h);
           });
  }

  void unmounted() { log->lines.emplace_back("unmounted"); }
};

struct maybe_slow {
  state<bool> shows{true};
  journal log;

  element view() {
    return div({when(*shows, [this] { return component<slow_row>({.log = &log}); })});
  }
};

// Its own journal, handed to a slow row: as the panel goes, it copies there what the row wrote, which only the row's
// task can have ended.
struct slow_panel {
  struct props {
    journal *log = nullptr;
  };

  struct witness {
    journal *to = nullptr;
    journal const *from = nullptr;

    witness() = default;
    witness(witness const &) = delete;
    witness(witness &&) = delete;
    witness &operator=(witness const &) = delete;
    witness &operator=(witness &&) = delete;

    ~witness() {
      if (to == nullptr) {
        return;
      }

      to->lines.insert(to->lines.end(), from->lines.begin(), from->lines.end());
      to->lines.emplace_back("panel gone");
    }
  };

  journal mine;
  witness last;

  element view(props const &given) {
    last.to = given.log;
    last.from = &mine;
    return component<slow_row>({.log = &mine});
  }
};

struct maybe_panel {
  state<bool> shows{true};
  journal log;

  element view() {
    return div({when(*shows, [this] { return component<slow_panel>({.log = &log}); })});
  }
};

// A count with no props.
struct counter {
  state<int> count;

  element view() {
    return p("count:" + std::to_string(*count)) | on_click([this] { count = *count + 1; });
  }
};

// A row as its whole view.
struct panel {
  struct props {
    std::string label;
    journal *log = nullptr;
  };

  journal *log = nullptr;

  element view(props const &given) {
    log = given.log;
    return component<row>({.label = given.label, .log = given.log});
  }

  void unmounted() { log->lines.emplace_back("unmounted panel"); }
};

// Two cells, side by side in the view of whoever places it.
struct cells {
  element view() {
    return list(std::vector<std::string>{"1", "2"}, [](std::string const &cell) { return p(cell); });
  }
};

// Components of three types, none keyed, a row at the same rank in its view and in the panel's.
struct mixed {
  state<int> views;
  journal log;

  element view() {
    return div({
        p("views") | on_click([this] { views = *views + 1; }),
        component<panel>({.label = "x", .log = &log}),
        component<counter>(),
        component<row>({.label = "y", .log = &log}),
        component<cells>(),
    });
  }
};

// Two unkeyed rows, the first shown or not.
struct two_rows {
  state<bool> first{true};
  journal log;

  element view() {
    return div({
        when(*first, [this] { return component<row>({.label = "a", .log = &log}); }),
        component<row>({.label = "b", .log = &log}),
    });
  }
};

struct stopping_row {
  struct props {
    std::function<task<>()> *stop = nullptr;
  };

  std::function<task<>()> *stop = nullptr;

  element view(props const &given) {
    stop = given.stop;
    return button("Stop") | on_click([this]() -> task<> { co_await (*stop)(); });
  }
};

struct stopping_parent {
  std::function<task<>()> *stop = nullptr;

  element view() { return div({component<stopping_row>({.stop = stop})}); }
};

// Shows a component, and goes after it.
struct page_with_row {
  journal log;

  element view() { return div({component<row>({.label = "a", .log = &log})}); }

  void unmounted() { log.lines.emplace_back("unmounted page"); }
};

// What its unmounted() throws is a failure of the app; the components beside it go all the same.
struct grumpy {
  struct props {};

  element view(props const & /*given*/) { return p("grumpy"); }

  void unmounted() { throw std::runtime_error{"will not go"}; }
};

struct grumpy_and_row {
  state<bool> shows{true};
  journal log;

  element view() {
    return div(
        {when(*shows, [this] { return div({component<grumpy>({}), component<row>({.label = "a", .log = &log})}); })});
  }
};

// A handler reading its props by reference after a wait, while the parent gives others.
struct reader {
  struct props {
    std::string label;
    std::vector<std::string> *read = nullptr;
  };

  element view(props const &given) {
    return button(given.label) | on_click([&given]() -> task<> {
             co_await quoll::async::sleep_for(1min);
             given.read->push_back(given.label);
           });
  }
};

struct reading {
  state<std::string> label{std::string{"old"}};
  std::vector<std::string> read;

  element view() { return div({component<reader>({.label = *label, .read = &read})}); }
};

// Its view is a fragment: the key of the component goes on each of its nodes.
struct cell_pair {
  struct props {
    std::string label;
  };

  element view(props const &given) {
    return list(std::vector<std::string>{given.label + "1", given.label + "2"},
                [](std::string const &cell) { return li(cell); });
  }
};

struct cell_pairs {
  state<std::vector<std::string>> order{std::vector<std::string>{"a", "b"}};

  element view() {
    return ul(
        {list(order, [](std::string const &label) { return component<cell_pair>({.label = label}) | key(label); })});
  }
};

// A renderer failing its next insert once told to: a render that fails past the views.
struct failing_renderer {
  memory_renderer shown;
  bool *fails = nullptr;

  void create_element(node_id id, std::string_view tag) { shown.create_element(id, tag); }

  void create_text(node_id id, std::string_view content) { shown.create_text(id, content); }

  void set_text(node_id id, std::string_view content) { shown.set_text(id, content); }

  void set_class(node_id id, std::string_view classes) { shown.set_class(id, classes); }

  void set_attribute(node_id id, std::string_view name, std::string_view value) {
    shown.set_attribute(id, name, value);
  }

  void remove_attribute(node_id id, std::string_view name) { shown.remove_attribute(id, name); }

  void set_value(node_id id, std::string_view value) { shown.set_value(id, value); }

  void listen(node_id id, std::string_view name) { shown.listen(id, name); }

  void unlisten(node_id id, std::string_view name) { shown.unlisten(id, name); }

  void insert(node_id parent, node_id child, node_id before) {
    if (*fails) {
      *fails = false;
      throw std::runtime_error{"no room"};
    }

    shown.insert(parent, child, before);
  }

  void remove(node_id id) { shown.remove(id); }
};

// Its view is a fragment with a node without a key, at position 0, and one with the key 0.
struct headed {
  struct props {};

  element view(props const & /*given*/) {
    return list(std::vector<int>{0, 1}, [](int item) { return item == 0 ? p("header") : li("first") | key(0); });
  }
};

// Its view has a key of its own, as its siblings' do.
struct keyed_item {
  element view() { return li("item") | key("1"); }
};

// Viewed twice: the reconciler then looks at the keys of the nodes it showed.
struct colliding {
  state<int> views;

  element view() { return ul({component<headed>({}) | key("k"), component<keyed_item>(), component<keyed_item>()}); }
};

// Gives its child a pointer into its own props.
struct quoting_child {
  struct props {
    std::string const *text = nullptr;
    std::vector<std::string> *read = nullptr;
  };

  element view(props const &given) {
    return button("quote") | on_click([&given]() -> task<> {
             co_await quoll::async::sleep_for(1min);
             given.read->push_back(*given.text);
           });
  }
};

struct quoting {
  struct props {
    std::string text;
    std::vector<std::string> *read = nullptr;
  };

  element view(props const &given) { return component<quoting_child>({.text = &given.text, .read = given.read}); }
};

struct quoted {
  state<std::string> label{std::string{"an old label, too long for a short string"}};
  std::vector<std::string> read;

  element view() { return div({component<quoting>({.text = *label, .read = &read})}); }
};

// Says when it is destroyed.
struct farewell {
  journal *to = nullptr;

  farewell() = default;
  farewell(farewell const &) = delete;
  farewell(farewell &&) = delete;
  farewell &operator=(farewell const &) = delete;
  farewell &operator=(farewell &&) = delete;

  ~farewell() {
    if (to != nullptr) {
      to->lines.emplace_back("gone");
    }
  }
};

// A component with one of its own, which holds it.
struct witnessed {
  struct props {
    journal *log = nullptr;
  };

  farewell bye;

  element view(props const &given) {
    bye.to = given.log;
    return div({component<counter>()});
  }
};

struct witnessing {
  journal *log = nullptr;

  element view() { return div({component<witnessed>({.log = log})}); }
};

struct refusing_executor {
  virtual_time::executor inner;
  bool *fail;

  void post(quoll::async::work_item &item) const {
    if (*fail) {
      throw std::runtime_error{"no more"};
    }

    inner.post(item);
  }

  [[nodiscard]] bool operator==(refusing_executor const &) const noexcept = default;
};

// Its view makes the executor refuse what comes next: its own mounted() cannot start.
struct refuser {
  struct props {
    bool *fail = nullptr;
    journal *log = nullptr;
  };

  journal *log = nullptr;

  element view(props const &given) {
    log = given.log;
    *given.fail = true;
    return p("refuser");
  }

  task<> mounted() {
    log->lines.emplace_back("mounted refuser");
    co_return;
  }
};

struct refusing_page {
  bool *fail = nullptr;
  journal log;

  element view() {
    return div({component<refuser>({.fail = fail, .log = &log}), component<row>({.label = "a", .log = &log})});
  }
};

static_assert(ComponentWithProps<row>);
static_assert(MountedComponent<row>);
static_assert(UnmountedComponent<row>);
static_assert(!Component<row>);
static_assert(Component<counter>);
static_assert(!ComponentWithProps<counter>);
static_assert(!MountedComponent<counter>);
static_assert(!UnmountedComponent<counter>);

} // namespace

TEST_CASE("component: a component of the view gets its props, and keeps its states as its parent views again",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<rows> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // The parent views again: the row keeps its count.
    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>b:0</button></ul></div>");
}

TEST_CASE("component: a keyed component of a list keeps its states with its item as the list moves",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<rows> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("b:0"), "click");
    co_await settle();

    instance.component().labels = std::vector<std::string>{"b", "c", "a"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() ==
          "<div><p>views</p><ul><button>b:1</button><button>c:0</button><button>a:0</button></ul></div>");
}

TEST_CASE("component: mounted() runs once a component first shows, unmounted() as it goes", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<rows> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().labels = std::vector<std::string>{"b"};
    co_await settle();
    // Viewed again with b still there: nothing comes or goes.
    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // b goes as the app stops.
  REQUIRE(log == std::vector<std::string>{"mounted a", "mounted b", "unmounted a", "unmounted b"});
}

TEST_CASE("component: one that goes has its tasks stopped, and stays until they have ended", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<maybe_slow> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.component().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"unmounted", "handler ended"});
  REQUIRE(time.now() < 1h);
  REQUIRE(shown.html() == "<div></div>");
}

TEST_CASE("component: to_html writes the views of the components of a view", "[ui][components]") {
  rows page;

  REQUIRE(to_html(page.view()) == "<div><p>views</p><ul><button>a:0</button><button>b:0</button></ul></div>");
}

TEST_CASE("component: one decorated otherwise than with a key is refused", "[ui][components]") {
  journal log;
  auto const decorated = component<row>({.label = "a", .log = &log}) | tw("p-2");

  REQUIRE_THROWS_AS(to_html(decorated), std::logic_error);
}

TEST_CASE("component: components in components, of several types, keep their states, and go after their children",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<mixed> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    REQUIRE(shown.html() ==
            "<div><p>views</p><button>x:0</button><p>count:0</p><button>y:0</button><p>1</p><p>2</p></div>");

    instance.dispatch(shown.find_text("x:0"), "click");
    instance.dispatch(shown.find_text("count:0"), "click");
    instance.dispatch(shown.find_text("count:0"), "click");
    co_await settle();

    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() ==
          "<div><p>views</p><button>x:1</button><p>count:2</p><button>y:0</button><p>1</p><p>2</p></div>");
  REQUIRE(log == std::vector<std::string>{"mounted x", "mounted y", "unmounted x", "unmounted panel", "unmounted y"});
}

TEST_CASE("component: an unkeyed component keeps its place by rank among those of its type", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<two_rows> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // b is first now: it takes the place, and the states, of the first row.
    instance.component().first = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><button>b:1</button></div>");
  REQUIRE(log == std::vector<std::string>{"mounted a", "mounted b", "unmounted b", "unmounted b"});
}

TEST_CASE("component: a view that throws changes nothing, and the next one starts from the last shown",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app<rows> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // c is made, then boom throws: c never shows, and b stays.
    instance.component().labels = std::vector<std::string>{"a", "c", "boom"};
    co_await settle();
    REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>b:0</button></ul></div>");

    instance.component().labels = std::vector<std::string>{"a", "c"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>c:0</button></ul></div>");
  REQUIRE(log ==
          std::vector<std::string>{"mounted a", "mounted b", "unmounted b", "mounted c", "unmounted a", "unmounted c"});
}

TEST_CASE("component: stop() from the task of a component fails rather than wait for itself", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app<stopping_parent> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};
  std::function<task<>()> stop = [&] { return instance.stop(); };

  instance.component().stop = &stop;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
}

TEST_CASE("component: one that goes stays, with its parent, until the tasks of its children have ended",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<maybe_panel> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.component().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"unmounted", "handler ended", "panel gone"});
  REQUIRE(shown.html() == "<div></div>");
}

TEST_CASE("component: the app's own unmounted() runs as it stops, after those of the components of its view",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<page_with_row> instance{time.get_executor(), shown};
  auto const &log = instance.component().log.lines;

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(log == std::vector<std::string>{"mounted a", "unmounted a", "unmounted page"});
}

TEST_CASE("component: what an unmounted() throws goes to on_error, and the others go all the same",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app<grumpy_and_row> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.component().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(log == std::vector<std::string>{"mounted a", "unmounted a"});
  REQUIRE(shown.html() == "<div></div>");
}

TEST_CASE("component: a handler keeps the props of the view that made it until its task ends", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<reading> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The parent gives other props while the handler waits.
    instance.component().label = std::string{"new"};
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().read == std::vector<std::string>{"old"});
  REQUIRE(shown.html() == "<div><button>new</button></div>");
}

TEST_CASE("component: the nodes of a keyed component whose view is a fragment move with it", "[ui][components]") {
  virtual_time time;
  memory_renderer shown;
  test_app<cell_pairs> instance{time.get_executor(), shown};
  node_id before{};
  node_id after{};

  auto const act = [&]() -> task<> {
    before = shown.find_text("a2");
    instance.component().order = std::vector<std::string>{"b", "a"};
    co_await settle();
    after = shown.find_text("a2");
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<ul><li>b1</li><li>b2</li><li>a1</li><li>a2</li></ul>");
  REQUIRE(before == after);
}

TEST_CASE("component: a render failing past the views changes no component, and the next one makes them again",
          "[ui][components]") {
  virtual_time time;
  bool fails = false;
  failing_renderer const shown{.fails = &fails};
  std::exception_ptr failure;
  app<rows, failing_renderer> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};
  auto const &log = instance.component().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    fails = true;
    instance.component().labels = std::vector<std::string>{"a", "c"};
    co_await settle();

    instance.component().labels = std::vector<std::string>{"a", "c"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(shown.shown.html() == "<div><p>views</p><ul><button>a:0</button><button>c:0</button></ul></div>");
  REQUIRE(log ==
          std::vector<std::string>{"mounted a", "mounted b", "unmounted b", "mounted c", "unmounted a", "unmounted c"});
}

TEST_CASE("component: the keys of the views of components never meet those of their siblings, nor positions",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<colliding> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().views = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<ul><p>header</p><li>first</li><li>item</li><li>item</li></ul>");
}

TEST_CASE("component: a pointer into the props of its parent stays good for a handler of a component",
          "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<quoted> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.component().label = std::string{"a new label"};
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().read == std::vector<std::string>{"an old label, too long for a short string"});
}

TEST_CASE("component: an app destroyed without its stop leaves no component behind", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  journal log;

  {
    test_app<witnessing> instance{time.get_executor(), shown};

    instance.component().log = &log;

    auto const start = [&]() -> task<int> {
      instance.start();
      co_await settle();
      co_return 0;
    };

    REQUIRE(quoll::test::run(time, start()) == 0);
    REQUIRE(log.lines.empty());
  }

  REQUIRE(log.lines == std::vector<std::string>{"gone"});
}

TEST_CASE("component: a mounted() that cannot start is told to on_error, and the others start", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  bool fail = false;
  std::exception_ptr failure;
  app<refusing_page, memory_renderer> instance{
      refusing_executor{time.get_executor(), &fail}, shown, {.on_error = [&](std::exception_ptr error) {
        fail = false;
        failure = std::move(error);
      }}};
  auto const &log = instance.component().log.lines;

  instance.component().fail = &fail;

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(log == std::vector<std::string>{"mounted a", "unmounted a"});
}
