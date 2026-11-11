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
#include <quoll/emscripten/dom_renderer.hpp>
#include <quoll/emscripten/mount.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <emscripten.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

// tests/fake_dom.js
extern "C" {
void quoll_test_dom_reset();
void quoll_test_dom_html(char *out, std::size_t size);
void quoll_test_dom_click(char const *text);
void quoll_test_dom_type(char const *text);
void quoll_test_widgets_install();
void quoll_test_widgets_log(char *out, std::size_t size);
void quoll_test_widgets_emit(char const *name, char const *value);
void quoll_test_widgets_emit_circular();
void quoll_test_widgets_release();
void quoll_test_widgets_emit_function();
}

// Not in an unnamed namespace: Glaze, under Clang, reflects only types with linkage.
namespace dom_widgets {

using namespace quoll::ui;

struct chart_props {
  std::vector<int> values;
};

struct picked {
  int index = 0;
};

inline constexpr auto fake_chart = widget<chart_props>("./fake_chart.js", "canvas");

// Charts whose modules fail: one the registry has not, one whose mount throws.
inline constexpr auto missing_chart = widget<chart_props>("./missing.js", "canvas");
inline constexpr auto failing_chart = widget<chart_props>("./failing_chart.js", "canvas");

struct failing_charts {
  element view() { return div({missing_chart({}), failing_chart({})}); }
};

// A chart, given more values or hidden by its buttons, and what it emits.
struct charting {
  static inline std::vector<int> picks{};

  state<std::vector<int>> values{std::vector<int>{1}};
  state<bool> shows{true};

  element view() {
    auto const pick = [](picked event) { picks.push_back(event.index); };

    auto const more = [this] { values.update([](auto &all) { all.push_back(2); }); };

    auto const hide = [this] { shows = false; };

    auto const chart = [&] { return fake_chart({.values = *values}) | on("picked", pick); };

    return div({button("more") | on_click(more), button("hide") | on_click(hide), when(*shows, chart)});
  }
};

} // namespace dom_widgets

namespace {

using namespace quoll::ui;

std::string widget_log() {
  std::array<char, 4096> buffer{};

  quoll_test_widgets_log(buffer.data(), buffer.size());
  return buffer.data();
}

std::string page() {
  std::array<char, 4096> buffer{};

  quoll_test_dom_html(buffer.data(), buffer.size());
  return buffer.data();
}

// Lets what is pending on the event loop run: the tasks of the app, posted as microtasks.
void settle() { emscripten_sleep(1); }

struct counter {
  state<int> count;

  element view() {
    return div({
               span("count: " + std::to_string(*count)) | tw("text-sm"),
               button("Add") | on_click([this] { count = *count + 1; }),
           }) |
           tw("flex");
  }
};

struct list_page {
  state<std::vector<std::string>> items;
  state<std::string> draft;

  element view() {
    return div({
        input() | bind(draft) | attribute("placeholder", "note"),
        button("Add") | on_click([this] {
          items.update([this](auto &all) { all.push_back(*draft); });
          draft.set({});
        }),
        ul({list(items,
                 [this](std::string const &item) {
                   return li(item) | key(item) |
                          on_click([this, item] { items.update([&item](auto &all) { std::erase(all, item); }); });
                 })}),
    });
  }
};

// mounted() fails once the first view is in the page, its listeners with it.
struct failing_after_view {
  static inline bool fail = true;

  state<int> count;

  element view() {
    return button("Add " + std::to_string(*count)) | on_click([this] { count = *count + 1; });
  }

  quoll::async::task<> mounted() {
    if (fail) {
      throw std::runtime_error{"not yet"};
    }

    return nothing();
  }

  static quoll::async::task<> nothing() { co_return; }
};

// The first view asks for another render, then fails: that render is pending on the event loop.
struct changing_then_failing {
  state<int> count;
  static inline int views = 0;

  element view() {
    ++views;
    count = 1;
    throw std::runtime_error{"not yet"};
  }
};

struct failing_at_first {
  static inline bool fail = true;

  element view() {
    if (fail) {
      throw std::runtime_error{"not yet"};
    }

    return p("shown");
  }
};

} // namespace

TEST_CASE("mount shows a component in the page, and events reach it", "[emscripten][ui]") {
  quoll_test_dom_reset();
  quoll::emscripten::mount<counter>();

  REQUIRE(page() == R"(<div class="flex"><span class="text-sm">count: 0</span><button>Add</button></div>)");

  quoll_test_dom_click("Add");
  settle();

  REQUIRE(page() == R"(<div class="flex"><span class="text-sm">count: 1</span><button>Add</button></div>)");
}

TEST_CASE("mount: inputs, lists and removed nodes in the page", "[emscripten][ui]") {
  quoll_test_dom_reset();
  quoll::emscripten::mount<list_page>();

  quoll_test_dom_type("milk");
  settle();
  quoll_test_dom_click("Add");
  settle();
  quoll_test_dom_type("tea");
  settle();
  quoll_test_dom_click("Add");
  settle();

  REQUIRE(
      page() ==
      R"(<div><input placeholder="note" value=""></input><button>Add</button><ul><li>milk</li><li>tea</li></ul></div>)");

  quoll_test_dom_click("milk");
  settle();

  REQUIRE(page() ==
          R"(<div><input placeholder="note" value=""></input><button>Add</button><ul><li>tea</li></ul></div>)");
}

TEST_CASE("dom_renderer: nothing matching the selector is an error", "[emscripten][ui]") {
  quoll_test_dom_reset();

  REQUIRE_THROWS_AS(quoll::emscripten::dom_renderer{"#missing"}, std::invalid_argument);
}

TEST_CASE("mount: a failed start leaves nothing in the page, and can be tried again", "[emscripten][ui]") {
  quoll_test_dom_reset();

  REQUIRE_THROWS_AS(quoll::emscripten::mount<failing_after_view>(), std::runtime_error);
  // At once: a retry in the same tick shows one interface.
  REQUIRE(page().empty());

  failing_after_view::fail = false;
  quoll::emscripten::mount<failing_after_view>();
  REQUIRE(page() == "<button>Add 0</button>");

  settle();
  REQUIRE(page() == "<button>Add 0</button>");
}

TEST_CASE("mount: a render pending when the first view fails does not outlive the app", "[emscripten][ui]") {
  quoll_test_dom_reset();

  REQUIRE_THROWS_AS(quoll::emscripten::mount<changing_then_failing>(), std::runtime_error);
  settle();

  REQUIRE(page().empty());
  REQUIRE(changing_then_failing::views == 1);
}

TEST_CASE("mount: a failed first view can be tried again, a second mount cannot", "[emscripten][ui]") {
  quoll_test_dom_reset();

  REQUIRE_THROWS_AS(quoll::emscripten::mount<failing_at_first>(), std::runtime_error);

  failing_at_first::fail = false;
  quoll::emscripten::mount<failing_at_first>();

  REQUIRE(page() == "<p>shown</p>");
  REQUIRE_THROWS_AS(quoll::emscripten::mount<failing_at_first>(), std::logic_error);
}

TEST_CASE("mount: a widget's module, from the page's registry, is mounted, updated and unmounted",
          "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();
  quoll::emscripten::mount<dom_widgets::charting>();
  settle();

  REQUIRE(widget_log() == R"(mount canvas {"values":[1]})");

  // What it emits reaches its function, read from JSON, with a field it does not declare.
  quoll_test_widgets_emit("picked", R"({"index":2,"dataset":0})");
  settle();
  REQUIRE(dom_widgets::charting::picks == std::vector<int>{2});

  quoll_test_dom_click("more");
  settle();
  REQUIRE(widget_log() == "mount canvas {\"values\":[1]}\nupdate {\"values\":[1,2]}");

  // Unmounted, it emits to nothing.
  quoll_test_dom_click("hide");
  settle();
  quoll_test_widgets_emit("picked", R"({"index":3})");
  settle();

  REQUIRE(widget_log() == "mount canvas {\"values\":[1]}\nupdate {\"values\":[1,2]}\nunmount");
  REQUIRE(dom_widgets::charting::picks == std::vector<int>{2});
  REQUIRE(page() == "<div><button>more</button><button>hide</button></div>");
}

TEST_CASE("dom_renderer: a widget unmounted while its module loads is never mounted", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  quoll::emscripten::dom_renderer renderer{"#app"};

  renderer.create_element(1, "canvas");
  renderer.insert(0, 1, 0);
  renderer.mount_widget(1, "./fake_chart.js", R"({"values":[]})");
  renderer.unmount_widget(1);
  settle();

  REQUIRE(widget_log().empty());
  renderer.close();
}

TEST_CASE("dom_renderer: a widget whose module the registry has not is refused", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  quoll::emscripten::dom_renderer renderer{"#app"};

  renderer.create_element(1, "canvas");
  renderer.insert(0, 1, 0);

  REQUIRE_THROWS_AS(renderer.mount_widget(1, "./missing.js", "{}"), std::invalid_argument);
  renderer.close();
}

TEST_CASE("dom_renderer: props given while its module loads are those it mounts with", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  quoll::emscripten::dom_renderer renderer{"#app"};

  renderer.create_element(1, "canvas");
  renderer.insert(0, 1, 0);
  renderer.mount_widget(1, "./fake_chart.js", R"({"values":[1]})");
  renderer.update_widget(1, R"({"values":[2]})");
  settle();

  REQUIRE(widget_log() == R"(mount canvas {"values":[2]})");
  renderer.close();
  REQUIRE(widget_log() == "mount canvas {\"values\":[2]}\nunmount");
}

namespace {

// A renderer on #app with one canvas, node 1, and what reaches its owner.
struct canvas_renderer {
  quoll::emscripten::dom_renderer renderer{"#app"};
  int events = 0;
  std::vector<std::string> failures;

  canvas_renderer() {
    renderer.create_element(1, "canvas");
    renderer.insert(0, 1, 0);

    renderer.on_event([](void *owner, quoll::ui::node_id, char const *,
                         char const *) { ++static_cast<canvas_renderer *>(owner)->events; },
                      this);
    renderer.on_failure(
        [](void *owner, char const *message) { static_cast<canvas_renderer *>(owner)->failures.emplace_back(message); },
        this);
  }

  canvas_renderer(canvas_renderer const &) = delete;
  canvas_renderer(canvas_renderer &&) = delete;
  canvas_renderer &operator=(canvas_renderer const &) = delete;
  canvas_renderer &operator=(canvas_renderer &&) = delete;

  ~canvas_renderer() { renderer.close(); }
};

} // namespace

TEST_CASE("dom_renderer: a widget's update or unmount that throws throws", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./throwing_chart.js", "{}");
  settle();

  REQUIRE_THROWS_AS(shown.renderer.update_widget(1, "{}"), std::runtime_error);
  REQUIRE_THROWS_AS(shown.renderer.unmount_widget(1), std::runtime_error);
  // Gone all the same.
  REQUIRE_NOTHROW(shown.renderer.unmount_widget(1));
}

TEST_CASE("dom_renderer: props given while a mount awaits reach it once it is done", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./async_chart.js", R"({"values":[1]})");
  settle();
  shown.renderer.update_widget(1, R"({"values":[2]})");
  quoll_test_widgets_release();
  settle();

  REQUIRE(widget_log() == "mount canvas {\"values\":[1]}\nupdate {\"values\":[2]}");
}

TEST_CASE("dom_renderer: one unmounted while its mount awaits has what it made unmounted", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./async_chart.js", R"({"values":[1]})");
  settle();
  shown.renderer.unmount_widget(1);
  quoll_test_widgets_release();
  settle();

  REQUIRE(widget_log() == "mount canvas {\"values\":[1]}\nunmount");
}

TEST_CASE("dom_renderer: a node removed takes the widgets within it out, and one mounted again its first",
          "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.create_element(2, "div");
  shown.renderer.create_element(3, "canvas");
  shown.renderer.insert(2, 3, 0);
  shown.renderer.insert(0, 2, 0);
  shown.renderer.mount_widget(3, "./fake_chart.js", R"({"values":[3]})");
  shown.renderer.mount_widget(1, "./fake_chart.js", R"({"values":[1]})");
  settle();
  shown.renderer.mount_widget(1, "./fake_chart.js", R"({"values":[2]})");
  settle();
  shown.renderer.remove(2);

  REQUIRE(widget_log() == "mount canvas {\"values\":[3]}\nmount canvas {\"values\":[1]}\nunmount\n"
                          "mount canvas {\"values\":[2]}\nunmount");
}

TEST_CASE("dom_renderer: a mount that fails is told to on_failure", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./failing_chart.js", "{}");
  settle();

  REQUIRE(shown.failures.size() == 1);
  REQUIRE(shown.failures.front().find("no canvas here") != std::string::npos);
}

TEST_CASE("dom_renderer: what JSON cannot hold, emitted, reaches nothing", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./fake_chart.js", "{}");
  settle();
  quoll_test_widgets_emit_circular();
  settle();
  quoll_test_widgets_emit_function();
  settle();
  quoll_test_widgets_emit("picked", R"({"index":1})");
  settle();

  REQUIRE(shown.events == 1);
}

TEST_CASE("mount: a widget's module missing, or failing to mount, reaches on_error", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  static std::vector<std::string> failures;

  quoll::ui::app_options options{.on_error = [](std::exception_ptr failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      failures.emplace_back(error.what());
    }
  }};

  quoll::emscripten::mount<dom_widgets::failing_charts>("#app", std::move(options));
  settle();

  REQUIRE(failures.size() == 2);
  REQUIRE(failures[0].find("./missing.js") != std::string::npos);
  REQUIRE(failures[1].find("no canvas here") != std::string::npos);
}

TEST_CASE("dom_renderer: an unmount failing once a mount it waited for is done is told to on_failure",
          "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  canvas_renderer shown;

  shown.renderer.mount_widget(1, "./async_throwing_chart.js", "{}");
  settle();
  shown.renderer.unmount_widget(1);
  quoll_test_widgets_release();
  settle();

  REQUIRE(shown.failures.size() == 1);
  REQUIRE(shown.failures.front().find("no unmount") != std::string::npos);
}

TEST_CASE("dom_renderer: once closed, a mount still running calls its owner no more", "[emscripten][ui][widget]") {
  quoll_test_dom_reset();
  quoll_test_widgets_install();

  std::vector<std::string> failures;
  quoll::emscripten::dom_renderer renderer{"#app"};

  renderer.on_failure(
      [](void *owner, char const *message) { static_cast<std::vector<std::string> *>(owner)->emplace_back(message); },
      &failures);
  renderer.create_element(1, "canvas");
  renderer.insert(0, 1, 0);
  renderer.mount_widget(1, "./async_throwing_chart.js", "{}");
  settle();

  // Its owner may go now: the deferred unmount fails to the console only.
  renderer.close();
  quoll_test_widgets_release();
  settle();

  REQUIRE(failures.empty());
}
