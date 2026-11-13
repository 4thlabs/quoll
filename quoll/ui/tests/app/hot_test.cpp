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

// Hot reload, in a program of its own: QUOLL_UI_HOT changes the signatures of tw(), text(), the tags and link_to(),
// which a program built with it calls in a library built without.

#include <quoll/async/sleep_for.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <chrono>
#include <cstdio>
#include <source_location>
#include <string>

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;

// The line of the literals below.
unsigned const view_line = std::source_location::current().line() + 8;

struct page_data {
  state<int> count;
};

constexpr component<> page = [] {
  static_cast<void>(*use_context<page_data>()->count);
  return button("Add") | tw("btn");
};

// The line of the literal of a component of a view, which a hot reload changes while nothing else does.
unsigned const child_line = std::source_location::current().line() + 2;

constexpr component<> child = [] { return p("Child"); };

constexpr component<> nested_page = [] { return div({child()}); };

// Links with a text, whose type QUOLL_UI_HOT changes: inline, they link against a library built without it, and their
// texts are replaced as any literal.
constexpr component<> other_page = [] { return p("The other page"); };

unsigned const links_line = std::source_location::current().line() + 4;

constexpr component<> linking_page = [] {
  return div({
      link_to(other_page, "Other"),
      link_to("/", "Home"),
      outlet(),
  });
};

inline constexpr auto linked_routes = routes(route<"/other", other_page>());

int failures = 0;

void check(std::string const &actual, std::string const &expected) {
  if (actual != expected) {
    std::fprintf(stderr, "expected %s\n     got %s\n", expected.c_str(), actual.c_str());
    ++failures;
  }
}

} // namespace

int main() {
  quoll::test::virtual_time time;
  quoll::test::memory_renderer const shown;
  app instance{time.get_executor(), shown, page, {.contexts = {provide<page_data>()}}};

  auto const *const file = std::source_location::current().file_name();
  auto const line = view_line;

  auto const session = [&]() -> task<int> {
    instance.start();
    check(shown.html(), R"(<button class="btn">Add</button>)");

    // Elsewhere: changes nothing.
    detail::hot_replace(file, line + 100, 1, "Other");
    co_await quoll::async::sleep_for(1ms);
    check(shown.html(), R"(<button class="btn">Add</button>)");

    // Clang, so Emscripten and the development server, places a literal at its opening quote; GCC, at the parenthesis
    // of the call.
#if defined(__clang__)
    detail::hot_replace(file, line, 17, "Ajouter");
    detail::hot_replace(file, line, 29, "btn btn-primary");
#else
    detail::hot_replace(file, line, 16, "Ajouter");
    detail::hot_replace(file, line, 28, "btn btn-primary");
#endif
    co_await quoll::async::sleep_for(1ms);
    check(shown.html(), R"(<button class="btn btn-primary">Ajouter</button>)");

    // And it stays replaced, when the view runs for another reason.
    instance.context<page_data>().count = 1;
    co_await quoll::async::sleep_for(1ms);
    check(shown.html(), R"(<button class="btn btn-primary">Ajouter</button>)");

    co_await instance.stop();
    co_return 0;
  };

  auto const done = quoll::test::run(time, session());

  // A component of a view, its props unchanged and its states too: a hot reload views it again all the same.
  quoll::test::virtual_time nested_time;
  quoll::test::memory_renderer const nested_shown;
  app nested{nested_time.get_executor(), nested_shown, nested_page};

  auto const nested_session = [&]() -> task<int> {
    nested.start();
    check(nested_shown.html(), "<div><p>Child</p></div>");

#if defined(__clang__)
    detail::hot_replace(file, child_line, 45, "Enfant");
#else
    detail::hot_replace(file, child_line, 44, "Enfant");
#endif
    co_await quoll::async::sleep_for(1ms);
    check(nested_shown.html(), "<div><p>Enfant</p></div>");

    co_await nested.stop();
    co_return 0;
  };

  auto const nested_done = quoll::test::run(nested_time, nested_session());

  quoll::test::virtual_time linked_time;
  quoll::test::memory_renderer const linked_shown;
  app linked{linked_time.get_executor(), linked_shown, linking_page, {.contexts = {routing<linked_routes>("/other")}}};

  auto const linked_session = [&]() -> task<int> {
    linked.start();
    check(
        linked_shown.html(),
        R"(<div><a data-quoll-link="" href="/other">Other</a><a data-quoll-link="" href="/">Home</a><p>The other page</p></div>)"
    );

#if defined(__clang__)
    detail::hot_replace(file, links_line, 27, "Autre");
    detail::hot_replace(file, links_line + 1, 20, "Accueil");
#else
    detail::hot_replace(file, links_line, 14, "Autre");
    detail::hot_replace(file, links_line + 1, 14, "Accueil");
#endif
    co_await quoll::async::sleep_for(1ms);
    check(
        linked_shown.html(),
        R"(<div><a data-quoll-link="" href="/other">Autre</a><a data-quoll-link="" href="/">Accueil</a><p>The other page</p></div>)"
    );

    co_await linked.stop();
    co_return 0;
  };

  auto const linked_done = quoll::test::run(linked_time, linked_session());

  return done.has_value() && nested_done.has_value() && linked_done.has_value() && failures == 0 ? 0 : 1;
}
