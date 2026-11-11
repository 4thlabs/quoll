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
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
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

template <class C>
using test_app = app<C, memory_renderer>;

struct chart_props {
  std::vector<std::string> labels;
  std::vector<int> values;
};

struct point_selected {
  int index = 0;
};

inline constexpr auto line_chart = widget<chart_props>("./widgets/line_chart.js", "canvas");

inline constexpr auto bar_chart = widget<chart_props>("./widgets/bar_chart.js", "canvas");

struct chart_page {
  state<std::vector<int>> values{std::vector<int>{1, 2}};
  state<bool> shows{true};
  state<bool> bars{false};
  state<int> unrelated;
  std::vector<int> selected;

  element view() {
    auto const select = [this](point_selected event) { selected.push_back(event.index); };

    auto const chart = [&] {
      auto const &shown = *bars ? bar_chart : line_chart;

      return shown({.labels = {"a", "b"}, .values = *values}) | tw("h-64") | on("point_selected", select);
    };

    return div({p(std::to_string(*unrelated)), when(*shows, chart)});
  }
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

inline std::string const mounted_line = R"(mount ./widgets/line_chart.js {"labels":["a","b"],"values":[1,2]})";

} // namespace widget_test

using namespace widget_test;

TEST_CASE("widget: mounted once its node is in place, with its props as JSON", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<chart_page> instance{time.get_executor(), shown};

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(shown.widget_log() == std::vector<std::string>{mounted_line, "unmount ./widgets/line_chart.js"});
}

TEST_CASE("widget: told of other props only, and unmounted before its node goes", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<chart_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    // Nothing of the chart changes.
    instance.component().unrelated = 1;
    co_await settle();

    instance.component().values = std::vector<int>{3};
    co_await settle();

    instance.component().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.widget_log() == std::vector<std::string>{mounted_line, R"(update {"labels":["a","b"],"values":[3]})",
                                                         "unmount ./widgets/line_chart.js"});
  REQUIRE(shown.html() == "<div><p>1</p></div>");
}

TEST_CASE("widget: another module at its place is another node", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<chart_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().bars = true;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.widget_log() ==
          std::vector<std::string>{mounted_line, "unmount ./widgets/line_chart.js",
                                   R"(mount ./widgets/bar_chart.js {"labels":["a","b"],"values":[1,2]})",
                                   "unmount ./widgets/bar_chart.js"});
}

TEST_CASE("widget: what it emits reaches its function, read from JSON", "[ui][widget]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app<chart_page> instance{
      time.get_executor(), shown, {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("canvas"), "point_selected", R"({"index":1})");
    co_await settle();

    instance.dispatch(shown.find("canvas"), "point_selected", "not JSON");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().selected == std::vector<int>{1});
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::invalid_argument);
}

TEST_CASE("widget: placed through a renderer that shows none, it throws", "[ui][widget]") {
  virtual_time time;
  app<chart_page, plain_renderer> instance{time.get_executor(), plain_renderer{}};

  auto const start_then_stop = [&]() -> task<int> {
    REQUIRE_THROWS_AS(instance.start(), std::logic_error);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
}

TEST_CASE("widget: to_html writes its node, empty, for the module to mount", "[ui][widget]") {
  chart_page page;

  REQUIRE(to_html(page.view()) == R"(<div><p>0</p><canvas class="h-64"></canvas></div>)");
}
