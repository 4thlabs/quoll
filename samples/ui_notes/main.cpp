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

// The notes of samples/notes in a browser. The contract is the same C++ header as the server's: a wrong call does not
// compile, and no TypeScript is generated. Served next to quoll_notes_server, under the same origin.

#include "notes_api.hpp"

#include <quoll/emscripten.hpp>
#include <quoll/rest.hpp>
#include <quoll/ui.hpp>

#include <exception>
#include <string>
#include <vector>

namespace {

using namespace quoll::ui;

using quoll::async::task;

struct notes_page {
  state<std::vector<notes::note>> items;
  state<std::string> draft;
  state<std::string> problem;
  quoll::rest::client<notes::api, quoll::emscripten::fetch_transport> api{quoll::emscripten::fetch_transport{""}};

  task<> mounted() {
    try {
      items = co_await api.call<notes::list>({});
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
      auto const created = co_await api.call<notes::create>({.text = sent});

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
      co_await api.call<notes::remove>(id);
      items.update([id](auto &all) { std::erase_if(all, [id](auto const &item) { return item.id == id; }); });
      problem.set({});
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  element note_row(notes::note const &item) {
    return li({
               span(item.text),
               button("Delete") | on_click([this, id = item.id]() -> task<> { co_await remove(id); }),
           }) |
           key(item.id);
  }

  element view() {
    return main_({
        h1("Notes"),
        div({
            input() | bind(draft) | attribute("placeholder", "A new note"),
            button("Add") | attribute("id", "add") | on_click([this]() -> task<> { co_await add(); }),
        }),
        when(!problem->empty(), [this] { return p(*problem) | attribute("role", "alert"); }),
        ul({list(items, [this](notes::note const &item) { return note_row(item); })}),
    });
  }
};

} // namespace

int main() { quoll::emscripten::mount<notes_page>(); }
