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

#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Not in an unnamed namespace: Glaze, under Clang, reflects only types with linkage.
namespace widget_test {

using namespace quoll::ui;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

struct chart_props {
  std::vector<std::string> labels;
  std::vector<int> values;
};

struct point_selected {
  int index = 0;
};

inline constexpr auto line_chart = widget<chart_props>("./widgets/line_chart.js", "canvas");

inline constexpr auto bar_chart = widget<chart_props>("./widgets/bar_chart.js", "canvas");

struct chart_page_data {
  state<std::vector<int>> values{std::vector<int>{1, 2}};
  state<bool> shows{true};
  state<bool> bars{false};
  state<int> unrelated;
  std::vector<int> selected;
};

constexpr component<> chart_page = [] {
  auto &self = use_context<chart_page_data>();

  auto const select = [&self](point_selected event) { self->selected.push_back(event.index); };

  auto const chart = [&] {
    auto const &shown = *self->bars ? bar_chart : line_chart;

    return shown({.labels = {"a", "b"}, .values = *self->values}) | tw("h-64") | on("point_selected", select);
  };

  return div({p(std::to_string(*self->unrelated)), when(*self->shows, chart)});
};

// A renderer that shows no widget.
struct plain_renderer {
  void create_element(node_id, std::string_view) {}

  void create_text(node_id, std::string_view) {}

  void set_text(node_id, std::string_view) {}

  void set_class(node_id, std::string_view) {}

  void set_attribute(node_id, std::string_view, std::string_view) {}

  void remove_attribute(node_id, std::string_view) {}

  void set_value(node_id, std::string_view) {}

  void set_checked(node_id, bool) {}

  void listen(node_id, std::string_view) {}

  void unlisten(node_id, std::string_view) {}

  void insert(node_id, node_id, node_id) {}

  void remove(node_id) {}
};

static_assert(Renderer<plain_renderer>);
static_assert(!WidgetRenderer<plain_renderer>);

static_assert(JsonEventFunction<void (*)(point_selected)>);
static_assert(JsonEventFunction<task<> (*)(point_selected const &)>);
static_assert(!JsonEventFunction<void (*)(std::string)>);
static_assert(!JsonEventFunction<void (*)()>);

// A component of the page, with a chart of its own.
struct chart_card_props {
  std::vector<int> values;

  [[nodiscard]] bool operator==(chart_card_props const &) const = default;
};

constexpr component<chart_card_props> chart_card = [](chart_card_props const &given) {
  return div({line_chart({.labels = {"a"}, .values = given.values})});
};

struct card_page_data {
  state<bool> shows{true};
};

constexpr component<> card_page = [] {
  auto &self = use_context<card_page_data>();

  auto const card = [] { return chart_card({.values = {1}}); };

  return div({when(*self->shows, card)});
};

// Keyed charts, reordered.
struct keyed_charts_data {
  state<std::vector<std::string>> names{std::vector<std::string>{"a", "b"}};
};

constexpr component<> keyed_charts = [] {
  auto &self = use_context<keyed_charts_data>();

  auto const chart = [](std::string const &name) {
    return line_chart({.labels = {name}, .values = {}}) | attribute("data-name", name) | key(name);
  };

  return div({list(self->names, chart)});
};

// A chart, and what it emits as it mounts or updates, seen by the function of the view that gave its props.
struct ready_page_data {
  state<int> view_count;
  std::vector<int> seen;
};

constexpr component<> ready_page = [] {
  auto &self = use_context<ready_page_data>();

  auto const ready = [&self, made_by = *self->view_count](point_selected event) {
    self->seen.push_back(made_by * 10 + event.index);
  };

  auto const chart = line_chart({.labels = {}, .values = {*self->view_count}});

  return div({chart | on("point_selected", ready)});
};

// Two charts, the first of which fails to mount.
constexpr component<> two_charts = [] {
  return div({bar_chart({.labels = {}, .values = {}}), line_chart({.labels = {}, .values = {}})});
};

// A chart that is given children in a later view.
struct crowding_later_data {
  state<bool> crowded{false};
};

constexpr component<> crowding_later = [] {
  auto &self = use_context<crowding_later_data>();

  auto chart = line_chart({.labels = {}, .values = {}});

  if (*self->crowded) {
    chart.children.push_back(p("mine"));
  }

  return div({chart});
};

// Two charts whose props change together.
struct changing_charts_data {
  state<int> value;
  state<bool> shows{true};
};

constexpr component<> changing_charts = [] {
  auto &self = use_context<changing_charts_data>();

  auto const second = [&self] { return line_chart({.labels = {"second"}, .values = {*self->value}}); };

  return div({line_chart({.labels = {"first"}, .values = {*self->value}}), when(*self->shows, second)});
};

// A chart given children, which are its module's.
constexpr component<> crowded_chart = [] {
  auto chart = line_chart({.labels = {}, .values = {}});

  chart.children.push_back(p("mine"));
  return chart;
};

// A chart, and an effect of its own, which a refusing executor cannot start.
constexpr component<> starting_page = [] {
  use_effect([]() -> task<> { co_return; });

  return div({line_chart({.labels = {}, .values = {}})});
};

// Shows widgets in memory, failing to mount the bar chart, and calling back as one mounts.
struct hooked_renderer {
  memory_renderer shown;
  std::function<void(node_id)> *mounting = nullptr;
  std::function<void(node_id)> *updating = nullptr;
  bool *fails_updates = nullptr;
  bool *fails_unmounts = nullptr;

  void create_element(node_id id, std::string_view tag) { shown.create_element(id, tag); }

  void create_text(node_id id, std::string_view content) { shown.create_text(id, content); }

  void set_text(node_id id, std::string_view content) { shown.set_text(id, content); }

  void set_class(node_id id, std::string_view classes) { shown.set_class(id, classes); }

  void set_attribute(node_id id, std::string_view name, std::string_view value) {
    shown.set_attribute(id, name, value);
  }

  void remove_attribute(node_id id, std::string_view name) { shown.remove_attribute(id, name); }

  void set_value(node_id id, std::string_view value) { shown.set_value(id, value); }

  void set_checked(node_id id, bool checked) { shown.set_checked(id, checked); }

  void listen(node_id id, std::string_view name) { shown.listen(id, name); }

  void unlisten(node_id id, std::string_view name) { shown.unlisten(id, name); }

  void insert(node_id parent, node_id child, node_id before) { shown.insert(parent, child, before); }

  void remove(node_id id) { shown.remove(id); }

  void mount_widget(node_id id, std::string_view module, std::string_view props) {
    if (module == bar_chart.module()) {
      throw std::runtime_error{"no bars here"};
    }

    shown.mount_widget(id, module, props);

    if (mounting != nullptr && *mounting) {
      (*mounting)(id);
    }
  }

  void update_widget(node_id id, std::string_view props) {
    if (fails_updates != nullptr && *fails_updates) {
      *fails_updates = false;
      throw std::runtime_error{"no update"};
    }

    shown.update_widget(id, props);

    if (updating != nullptr && *updating) {
      (*updating)(id);
    }
  }

  void unmount_widget(node_id id) {
    if (fails_unmounts != nullptr && *fails_unmounts) {
      *fails_unmounts = false;
      throw std::runtime_error{"no unmount"};
    }

    shown.unmount_widget(id);
  }
};

static_assert(WidgetRenderer<hooked_renderer>);

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

// A string, or what one converts to, is the value as it is; a noexcept function is one like the others.
static_assert(EventFunction<void (*)(std::optional<std::string>)>);
static_assert(!JsonEventFunction<void (*)(std::optional<std::string>)>);
static_assert(JsonEventFunction<void (*)(point_selected) noexcept>);

inline std::string const mounted_line = R"(mount ./widgets/line_chart.js {"labels":["a","b"],"values":[1,2]})";

} // namespace widget_test

using namespace widget_test;

TEST_CASE("widget: mounted once its node is in place, with its props as JSON", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, chart_page, {.contexts = {provide<chart_page_data>()}}};

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(shown.widget_log() == std::vector<std::string>{mounted_line, "unmount ./widgets/line_chart.js"});
}

TEST_CASE("widget: told of other props only, and unmounted before its node goes", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, chart_page, {.contexts = {provide<chart_page_data>()}}};

  auto const act = [&]() -> task<> {
    // Nothing of the chart changes.
    instance.context<chart_page_data>().unrelated = 1;
    co_await settle();

    instance.context<chart_page_data>().values = std::vector<int>{3};
    co_await settle();

    instance.context<chart_page_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.widget_log() ==
      std::vector<std::string>{
          mounted_line, R"(update {"labels":["a","b"],"values":[3]})", "unmount ./widgets/line_chart.js"
      }
  );
  REQUIRE(shown.html() == "<div><p>1</p></div>");
}

TEST_CASE("widget: another module at its place is another node", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, chart_page, {.contexts = {provide<chart_page_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<chart_page_data>().bars = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.widget_log() ==
      std::vector<std::string>{
          mounted_line, "unmount ./widgets/line_chart.js",
          R"(mount ./widgets/bar_chart.js {"labels":["a","b"],"values":[1,2]})", "unmount ./widgets/bar_chart.js"
      }
  );
}

TEST_CASE("widget: what it emits reaches its function, read from JSON", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app instance{
      time.get_executor(),
      shown,
      chart_page,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); },
       .contexts = {provide<chart_page_data>()}}
  };

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("canvas"), "point_selected", R"({"index":1})");
    co_await settle();

    instance.dispatch(shown.find("canvas"), "point_selected", "not JSON");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<chart_page_data>().selected == std::vector<int>{1});
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::invalid_argument);
}

TEST_CASE("widget: placed through a renderer that shows none, it throws", "[ui][widget]") {
  virtual_time time;
  app instance{time.get_executor(), plain_renderer{}, chart_page, {.contexts = {provide<chart_page_data>()}}};

  auto const start_then_stop = [&]() -> task<int> {
    REQUIRE_THROWS_AS(instance.start(), std::logic_error);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
}

TEST_CASE("widget: to_html writes its node, empty, for the module to mount", "[ui][widget]") {
  auto const page = div({p("0"), line_chart({.labels = {"a", "b"}, .values = {1, 2}}) | tw("h-64")});

  REQUIRE(to_html(page) == R"(<div><p>0</p><canvas class="h-64"></canvas></div>)");
}

TEST_CASE("widget: one of a component goes with it, and moves with its key", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, card_page, {.contexts = {provide<card_page_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<card_page_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.widget_log() ==
      std::vector<std::string>{
          R"(mount ./widgets/line_chart.js {"labels":["a"],"values":[1]})", "unmount ./widgets/line_chart.js"
      }
  );

  virtual_time keyed_time;
  memory_renderer const keyed_shown;
  test_app keyed{keyed_time.get_executor(), keyed_shown, keyed_charts, {.contexts = {provide<keyed_charts_data>()}}};
  node_id before{};
  node_id after{};

  std::string reordered;

  auto const reorder = [&]() -> task<> {
    before = keyed_shown.find("canvas");
    keyed.context<keyed_charts_data>().names = std::vector<std::string>{"b", "a"};
    co_await settle();
    after = keyed_shown.find("canvas");
    reordered = keyed_shown.html();
  };

  REQUIRE(quoll::test::run(keyed_time, session(keyed, reorder)) == 0);
  REQUIRE(reordered == R"(<div><canvas data-name="b"></canvas><canvas data-name="a"></canvas></div>)");
  // Moved, neither mounted again nor unmounted until the app stops.
  REQUIRE(keyed_shown.widget_log().size() == 4);
  REQUIRE(keyed_shown.widget_log()[2].starts_with("unmount"));
  REQUIRE(before == after);
}

TEST_CASE("widget: one that fails to mount is told to on_error, and the others mount", "[ui][widget]") {
  virtual_time time;
  hooked_renderer const shown;
  int failures = 0;
  app instance{time.get_executor(), shown, two_charts, {.on_error = [&](std::exception_ptr) { ++failures; }}};

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(failures == 1);
  REQUIRE(
      shown.shown.widget_log() ==
      std::vector<std::string>{
          R"(mount ./widgets/line_chart.js {"labels":[],"values":[]})", "unmount ./widgets/line_chart.js"
      }
  );
}

TEST_CASE("widget: what it emits as it mounts reaches its function", "[ui][widget]") {
  virtual_time time;
  std::function<void(node_id)> mounting;
  hooked_renderer const shown{.mounting = &mounting};
  app instance{time.get_executor(), shown, ready_page, {.contexts = {provide<ready_page_data>()}}};

  mounting = [&](node_id id) { instance.dispatch(id, "point_selected", R"({"index":3,"datasetIndex":0})"); };

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  // The field it does not declare is left.
  REQUIRE(instance.context<ready_page_data>().seen == std::vector<int>{3});
}

TEST_CASE("widget: given children, it is refused", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, crowded_chart};

  auto const start_then_stop = [&]() -> task<int> {
    REQUIRE_THROWS_AS(instance.start(), std::logic_error);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
  REQUIRE_THROWS_AS(to_html(crowded_chart()), std::logic_error);
}

TEST_CASE("widget: a start whose effect cannot start shows all the same, its widgets mounted", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  bool fail = true;
  int failures = 0;
  app instance{
      refusing_executor{time.get_executor(), &fail},
      shown,
      starting_page,
      {.on_error = [&failures](std::exception_ptr) { ++failures; }}
  };

  instance.start();
  REQUIRE(failures == 1);
  REQUIRE(shown.widget_log() == std::vector<std::string>{R"(mount ./widgets/line_chart.js {"labels":[],"values":[]})"});

  fail = false;

  auto const stop = [&]() -> task<int> {
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, stop()) == 0);
  REQUIRE(
      shown.widget_log() ==
      std::vector<std::string>{
          R"(mount ./widgets/line_chart.js {"labels":[],"values":[]})", "unmount ./widgets/line_chart.js"
      }
  );
}

TEST_CASE("widget: given children in a later view, the render is refused, and nothing changes", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app instance{
      time.get_executor(),
      shown,
      crowding_later,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); },
       .contexts = {provide<crowding_later_data>()}}
  };

  auto const act = [&]() -> task<> {
    instance.context<crowding_later_data>().crowded = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
  REQUIRE(shown.html() == "<div><canvas></canvas></div>");
  REQUIRE(
      shown.widget_log() ==
      std::vector<std::string>{
          R"(mount ./widgets/line_chart.js {"labels":[],"values":[]})", "unmount ./widgets/line_chart.js"
      }
  );
}

TEST_CASE("widget: an update or an unmount that fails is told to on_error, and the render goes on", "[ui][widget]") {
  virtual_time time;
  bool fails_updates = false;
  bool fails_unmounts = false;
  hooked_renderer const shown{.fails_updates = &fails_updates, .fails_unmounts = &fails_unmounts};
  int failures = 0;
  app instance{
      time.get_executor(),
      shown,
      changing_charts,
      {.on_error = [&](std::exception_ptr) { ++failures; }, .contexts = {provide<changing_charts_data>()}}
  };

  auto const act = [&]() -> task<> {
    // The first update asked fails; the other chart is updated all the same.
    fails_updates = true;
    instance.context<changing_charts_data>().value = 1;
    co_await settle();

    // The unmount of the second fails; it goes all the same.
    fails_unmounts = true;
    instance.context<changing_charts_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(failures == 2);
  REQUIRE(shown.shown.html() == "<div><canvas></canvas></div>");
  auto const log = shown.shown.widget_log();
  auto const updated = std::ranges::count_if(log, [](std::string const &line) { return line.starts_with("update"); });

  REQUIRE(updated == 1);
}

TEST_CASE("widget: what it emits as it updates reaches the function of the view that gave its props", "[ui][widget]") {
  virtual_time time;
  std::function<void(node_id)> updating;
  hooked_renderer const shown{.updating = &updating};
  app instance{time.get_executor(), shown, ready_page, {.contexts = {provide<ready_page_data>()}}};

  updating = [&](node_id id) { instance.dispatch(id, "point_selected", R"({"index":0})"); };

  auto const act = [&]() -> task<> {
    instance.context<ready_page_data>().view_count = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<ready_page_data>().seen == std::vector<int>{10});
}
