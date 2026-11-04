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

// Hot reload, in a program of its own: QUOLL_UI_HOT changes the signatures of tw(), text() and the tags.

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
unsigned const view_line = std::source_location::current().line() + 5;

struct page {
  state<int> count;

  element view() { return button("Add") | tw("btn"); }
};

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
  app<page, quoll::test::memory_renderer> instance{time.get_executor(), shown};

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
    detail::hot_replace(file, line, 34, "Ajouter");
    detail::hot_replace(file, line, 46, "btn btn-primary");
#else
    detail::hot_replace(file, line, 33, "Ajouter");
    detail::hot_replace(file, line, 45, "btn btn-primary");
#endif
    co_await quoll::async::sleep_for(1ms);
    check(shown.html(), R"(<button class="btn btn-primary">Ajouter</button>)");

    // And it stays replaced, when the view runs for another reason.
    instance.component().count = 1;
    co_await quoll::async::sleep_for(1ms);
    check(shown.html(), R"(<button class="btn btn-primary">Ajouter</button>)");

    co_await instance.stop();
    co_return 0;
  };

  auto const done = quoll::test::run(time, session());

  return done.has_value() && failures == 0 ? 0 : 1;
}
