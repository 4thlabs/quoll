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

#include "components/notes_page.hpp"
#include "client/notes_client.hpp"
#include "components/note_form.hpp"
#include "components/note_row.hpp"
#include "note.hpp"

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace notes_web {

using quoll::async::task;

namespace {

// What the page shows: the notes, and what went wrong last, none once a call succeeds. States of the page, which
// outlive its tasks.
struct notes_shown {
  state<std::vector<notes::note>> &items;
  state<std::string> &problem;
};

task<> load_notes(notes_shown shown) {
  try {
    shown.items = co_await notes_client::list();
  } catch (std::exception const &failure) {
    shown.problem = std::string{failure.what()};
  }
}

task<bool> add_note(notes_shown shown, std::string text) {
  try {
    auto const created = co_await notes_client::create(std::move(text));

    shown.items.update([&created](auto &all) { all.push_back(created); });
    shown.problem.set({});
    co_return true;
  } catch (std::exception const &failure) {
    shown.problem = std::string{failure.what()};
    co_return false;
  }
}

task<> remove_note(notes_shown shown, std::int64_t id) {
  try {
    co_await notes_client::remove(id);

    auto const removed = [id](notes::note const &item) { return item.id == id; };

    shown.items.update([&removed](auto &all) { std::erase_if(all, removed); });
    shown.problem.set({});
  } catch (std::exception const &failure) {
    shown.problem = std::string{failure.what()};
  }
}

} // namespace

constexpr component<> notes_page = [] {
  auto &items = use_state<std::vector<notes::note>>();
  auto &problem = use_state<std::string>();

  notes_shown const shown{.items = items, .problem = problem};

  // Loaded once, as the page first shows.
  use_effect([shown] { return load_notes(shown); });

  auto const on_add = [shown](std::string text) { return add_note(shown, std::move(text)); };

  // One callback, which every row shares.
  callback<void(std::int64_t)> const on_remove = [shown](std::int64_t id) { return remove_note(shown, id); };

  auto const row = [&on_remove](notes::note const &item) {
    return note_row({.item = item, .on_remove = on_remove}) | key(item.id);
  };

  auto const alert = [&problem] { return p(*problem) | tw("alert alert-error") | role("alert"); };

  auto const page = main_({
      h1("Notes") | tw("text-4xl font-bold"),
      note_form({.on_add = on_add}),
      when(!problem->empty(), alert),
      ul({list(items, row)}) | tw("flex flex-col gap-2"),
  });

  return page | tw("mx-auto flex max-w-xl flex-col gap-6 p-8");
};

} // namespace notes_web
