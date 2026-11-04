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
}

namespace {

using namespace quoll::ui;

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
