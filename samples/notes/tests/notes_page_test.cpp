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

// The page of samples/notes/web, run natively: the calls to the server are defined here, over notes in memory, in place
// of notes_client.cpp, which goes through fetch.

#include "client/notes_client.hpp"
#include "components/notes_page.hpp"
#include "note.hpp"

#include <quoll/async/task.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using notes_app = quoll::ui::app<memory_renderer>;

// What the server holds, for the test running.
struct fake_server {
  std::vector<notes::note> notes;
  std::int64_t next_id = 1;
  bool refuses = false;
};

fake_server server;

void type(notes_app &instance, memory_renderer const &shown, std::string text) {
  instance.dispatch(shown.find("input"), "input", std::move(text));
}

void click(notes_app &instance, memory_renderer const &shown, std::string_view label) {
  instance.dispatch(shown.find_text(label), "click");
}

} // namespace

namespace notes_client {

task<std::vector<notes::note>> list() { co_return server.notes; }

task<notes::note> create(std::string text) {
  if (server.refuses) {
    throw std::runtime_error{"the server is gone"};
  }

  notes::note created{.id = server.next_id++, .text = std::move(text)};

  server.notes.push_back(created);
  co_return created;
}

task<> remove(std::int64_t id) {
  std::erase_if(server.notes, [id](notes::note const &item) { return item.id == id; });
  co_return;
}

} // namespace notes_client

TEST_CASE(
    "notes web: the page shows the server's notes, adds what is typed once, and removes a note", "[samples][notes]"
) {
  server = {.notes = {{.id = 1, .text = "buy milk"}}, .next_id = 2};

  virtual_time time;
  memory_renderer const shown;
  notes_app instance{time.get_executor(), shown, notes_web::notes_page};
  std::string loaded;
  std::string added;

  auto const act = [&]() -> task<> {
    co_await settle();
    loaded = shown.html();

    type(instance, shown, "buy coffee");
    click(instance, shown, "Add");
    co_await settle();
    added = shown.html();

    // The input was emptied as the note left: a second click sends nothing.
    click(instance, shown, "Add");
    co_await settle();

    // The first Delete, the row shown first.
    click(instance, shown, "Delete");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loaded.contains("<span class=\"flex-1\">buy milk</span>"));
  CHECK(added.contains("buy milk"));
  CHECK(added.contains("buy coffee"));
  REQUIRE(server.notes.size() == 1);
  CHECK(server.notes.front().text == "buy coffee");
  CHECK(shown.html().contains("buy coffee"));
  CHECK_FALSE(shown.html().contains("buy milk"));
  CHECK_FALSE(shown.html().contains("role=\"alert\""));
}

TEST_CASE("notes web: a note the server refuses shows why, and comes back in the input", "[samples][notes]") {
  server = {.notes = {}, .next_id = 1, .refuses = true};

  virtual_time time;
  memory_renderer const shown;
  notes_app instance{time.get_executor(), shown, notes_web::notes_page};
  std::string refused;

  auto const act = [&]() -> task<> {
    co_await settle();
    type(instance, shown, "buy milk");
    click(instance, shown, "Add");
    co_await settle();
    refused = shown.html();

    // Nothing typed since: the click sends the note that came back.
    server.refuses = false;
    click(instance, shown, "Add");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(refused.contains("role=\"alert\""));
  CHECK(refused.contains("the server is gone"));
  REQUIRE(server.notes.size() == 1);
  CHECK(server.notes.front().text == "buy milk");
  CHECK_FALSE(shown.html().contains("role=\"alert\""));
}
