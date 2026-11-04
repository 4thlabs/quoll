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

// The page of the notes: what it shows and what it does, in a header. The source that changes all the time.

#ifndef QUOLL_SAMPLES_UI_NOTES_PAGE_HPP
#define QUOLL_SAMPLES_UI_NOTES_PAGE_HPP

#include "note.hpp"
#include "notes_client.hpp"

#include <quoll/ui.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace notes_ui {

using quoll::async::task;
using quoll::ui::element;
using quoll::ui::state;

struct notes_page {
  state<std::vector<notes::note>> items;
  state<std::string> draft;
  state<std::string> problem;

  task<> mounted() {
    try {
      items = co_await notes_client::list();
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  task<> add() {
    if (draft->empty()) {
      co_return;
    }

    // Out of the input as it leaves: what is typed meanwhile is the next note, and a second click sends nothing.
    auto const sent = *draft;

    draft.set({});

    try {
      auto const created = co_await notes_client::create(sent);

      items.update([&created](auto &all) { all.push_back(created); });
      problem.set({});
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};

      if (draft->empty()) {
        draft = sent;
      }
    }
  }

  task<> remove(std::int64_t id) {
    try {
      co_await notes_client::remove(id);
      items.update([id](auto &all) { std::erase_if(all, [id](auto const &item) { return item.id == id; }); });
      problem.set({});
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  element note_row(notes::note const &item) {
    using namespace quoll::ui;

    return li({
               span(item.text) | tw("flex-1"),
               button("Delete") | tw("btn btn-ghost btn-sm") |
                   on_click([this, id = item.id]() -> task<> { co_await remove(id); }),
           }) |
           tw("flex items-center gap-2 rounded-box bg-base-100 px-4 py-2 shadow-sm") | key(item.id);
  }

  element view() {
    using namespace quoll::ui;

    return main_({
               h1("Notes") | tw("text-4xl font-bold"),
               div({
                   input() | bind(draft) | tw("input input-bordered flex-1") | attribute("placeholder", "A new note"),
                   button("Add") | tw("btn btn-primary") | attribute("id", "add") |
                       on_click([this]() -> task<> { co_await add(); }),
               }) | tw("flex gap-2"),
               when(!problem->empty(),
                    [this] { return p(*problem) | tw("alert alert-error") | attribute("role", "alert"); }),
               ul({list(items, [this](notes::note const &item) { return note_row(item); })}) |
                   tw("flex flex-col gap-2"),
           }) |
           tw("mx-auto flex max-w-xl flex-col gap-6 p-8");
  }
};

} // namespace notes_ui

#endif // QUOLL_SAMPLES_UI_NOTES_PAGE_HPP
