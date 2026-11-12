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

namespace {

using namespace quoll::ui;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

template <class C>
using test_app = app<C, memory_renderer>;

// A row counting its views, with props comparable with ==.
struct counted_row {
  struct props {
    std::string label;
    int *views = nullptr;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  state<int> clicks;

  element view(props const &given) {
    ++*given.views;
    return button(given.label + ":" + std::to_string(*clicks)) | on_click([this] { clicks = *clicks + 1; });
  }
};

struct skipping_page {
  state<int> unrelated;
  state<std::string> label{std::string{"a"}};
  int views = 0;

  element view() {
    return div({p(std::to_string(*unrelated)), component<counted_row>({.label = *label, .views = &views})});
  }
};

// The same row, whose props have no ==.
struct uncompared_row {
  struct props {
    int *views = nullptr;
  };

  element view(props const &given) {
    ++*given.views;
    return p("uncompared");
  }
};

struct uncompared_page {
  state<int> unrelated;
  int views = 0;

  element view() { return div({p(std::to_string(*unrelated)), component<uncompared_row>({.views = &views})}); }
};

// A panel, its props comparable, holding a row of its own: the row changes while the panel's view is given again.
struct counted_panel {
  struct props {
    int *views = nullptr;
    int *row_views = nullptr;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) {
    ++*given.views;
    return div({component<counted_row>({.label = "inner", .views = given.row_views})});
  }
};

struct panel_page {
  state<int> unrelated;
  int panel_views = 0;
  int row_views = 0;

  element view() {
    return div(
        {p(std::to_string(*unrelated)), component<counted_panel>({.views = &panel_views, .row_views = &row_views})});
  }
};

// A row calling its page back, with what the page captured as it viewed.
struct calling_row {
  struct props {
    callback<void()> on_go;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) {
    return button("go") | on_click([&given] { return given.on_go(); });
  }
};

struct calling_page {
  state<int> count;
  std::vector<int> seen;

  element view() {
    return div(
        {p(std::to_string(*count)), component<calling_row>({.on_go = [this, n = *count] { seen.push_back(n); }})});
  }
};

// A panel of the page, with props of its own, giving its row a callback, and viewing again as its own count changes:
// the row's props compare equal, and it keeps the callback of the panel's first view.
struct calling_panel {
  struct props {
    std::vector<int> *seen = nullptr;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  state<int> count;

  element view(props const &given) {
    return div({p(std::to_string(*count)) | on_click([this] { count = *count + 1; }),
                component<calling_row>({.on_go = [seen = given.seen] { seen->push_back(1); }})});
  }
};

struct calling_panel_page {
  std::vector<int> seen;

  element view() { return div({component<calling_panel>({.seen = &seen})}); }
};

// A panel, its props comparable, holding a row whose props have none: as the panel's view is given again, so are
// the row's props, and the row is not viewed again either.
struct holding_panel {
  struct props {
    int *views = nullptr;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) { return div({component<uncompared_row>({.views = given.views})}); }
};

struct holding_page {
  state<int> unrelated;
  int views = 0;

  element view() { return div({p(std::to_string(*unrelated)), component<holding_panel>({.views = &views})}); }
};

// A component without props, with a state of its own, placed by a page that views again.
struct bare_counter {
  state<int> count;

  element view() {
    auto const add = [this] { count = *count + 1; };

    return button("count:" + std::to_string(*count)) | on_click(add);
  }
};

struct bare_page {
  state<int> unrelated;

  element view() { return div({p(std::to_string(*unrelated)), component<bare_counter>()}); }
};

// Keyed rows, their props equal, reordered as their page views again.
struct reordered_page {
  state<std::vector<std::string>> labels{std::vector<std::string>{"a", "b"}};
  int views = 0;

  element view() {
    auto const row = [this](std::string const &label) {
      return component<counted_row>({.label = label, .views = &views}) | key(label);
    };

    return ul({list(labels, row)});
  }
};

// Changes a state of its own in its first view, before its app links it.
struct readying {
  state<bool> ready;

  element view() {
    auto const was = *ready;

    // Shows only in another view.
    if (!was) {
      ready = true;
    }

    return p(was ? "ready" : "not yet");
  }
};

struct readying_page {
  element view() { return div({component<readying>()}); }
};

// A renderer failing its next insert once told to.
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

  void set_checked(node_id id, bool checked) { shown.set_checked(id, checked); }

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

struct failing_page {
  state<std::string> label{std::string{"a"}};
  state<bool> extra{false};
  int views = 0;

  element view() {
    return div({component<counted_row>({.label = *label, .views = &views}), when(*extra, [] { return p("extra"); })});
  }
};

} // namespace

TEST_CASE("skip: a component whose props are equal and whose states did not change is not viewed again", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<skipping_page> instance{time.get_executor(), shown};
  auto const &views = instance.component().views;

  auto const act = [&]() -> task<> {
    REQUIRE(views == 1);

    // The page views again: the row's props are the same.
    instance.component().unrelated = 1;
    co_await settle();
    REQUIRE(views == 1);
    REQUIRE(shown.html() == "<div><p>1</p><button>a:0</button></div>");

    // A handler of the view given again still runs; the row's own change views it again.
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
    REQUIRE(views == 2);

    // Other props view it again.
    instance.component().label = std::string{"b"};
    co_await settle();
    REQUIRE(views == 3);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><p>1</p><button>b:1</button></div>");
}

TEST_CASE("skip: one whose props have no == is viewed again with its parent", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<uncompared_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().unrelated = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().views == 2);
}

TEST_CASE("skip: a component of a view given again is viewed again as its states change", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<panel_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().unrelated = 1;
    co_await settle();
    REQUIRE(instance.component().panel_views == 1);
    REQUIRE(instance.component().row_views == 1);

    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().panel_views == 1);
  REQUIRE(instance.component().row_views == 2);
  REQUIRE(shown.html() == "<div><p>1</p><div><button>inner:1</button></div></div>");
}

TEST_CASE("skip: a callback of a component not viewed again calls what its parent's last view gave it", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<calling_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    // The page views again with another count; the row's props compare equal, as a callback does.
    instance.component().count = 1;
    co_await settle();

    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // What the callback captured as the row last viewed: capture only what does not change, or pass it as a prop.
  REQUIRE(instance.component().seen == std::vector<int>{0});
}

TEST_CASE("skip: a component whose view ran in a render that failed is viewed again in the next", "[ui][skip]") {
  virtual_time time;
  bool fails = false;
  failing_renderer const shown{.fails = &fails};
  app<failing_page, failing_renderer> instance{time.get_executor(), shown, {.on_error = [](std::exception_ptr) {}}};

  auto const act = [&]() -> task<> {
    co_await settle();

    // The row views with b, then the insert of the extra paragraph fails.
    fails = true;
    instance.component().label = std::string{"b"};
    instance.component().extra = true;
    co_await settle();

    // Again, with the same props as the view that did not show: the row views again.
    instance.component().extra = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().views == 3);
  REQUIRE(shown.shown.html() == "<div><button>b:0</button></div>");
}

TEST_CASE("skip: a callback a component gave before it viewed again still calls it, from a row not viewed again",
          "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<calling_panel_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    // The panel views again; the row, its props equal, keeps the callback of the panel's first view, which still works.
    instance.dispatch(shown.find("p"), "click");
    co_await settle();

    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().seen == std::vector<int>{1});
}

TEST_CASE("skip: a component of a view given again is not viewed again, even with props that have no ==",
          "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<holding_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().unrelated = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().views == 1);
}

TEST_CASE("skip: a component without props is viewed again only as its states change", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<bare_page> instance{time.get_executor(), shown};

  auto const act = [&]() -> task<> {
    instance.component().unrelated = 1;
    co_await settle();
    REQUIRE(shown.html() == "<div><p>1</p><button>count:0</button></div>");

    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><p>1</p><button>count:1</button></div>");
}

TEST_CASE("skip: keyed components not viewed again move with their items, their handlers with them", "[ui][skip]") {
  virtual_time time;
  memory_renderer shown;
  test_app<reordered_page> instance{time.get_executor(), shown};
  quoll::ui::node_id before{};
  quoll::ui::node_id after{};

  auto const act = [&]() -> task<> {
    before = shown.find_text("b:0");
    instance.component().labels = std::vector<std::string>{"b", "a"};
    co_await settle();
    after = shown.find_text("b:0");

    // The handler of b, given again, still counts b's clicks.
    instance.dispatch(after, "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.component().views == 3);
  REQUIRE(before == after);
  REQUIRE(shown.html() == "<ul><button>b:1</button><button>a:0</button></ul>");
}

TEST_CASE("skip: a state a component changes in its first view views it again", "[ui][skip]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<readying_page> instance{time.get_executor(), shown};

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(shown.html() == "<div><p>ready</p></div>");
}
