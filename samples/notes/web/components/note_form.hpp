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

// The input of a new note, which asks the page to add it.

#ifndef QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTE_FORM_HPP
#define QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTE_FORM_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <string>

namespace notes_web {

using quoll::ui::callback;
using quoll::ui::element;
using quoll::ui::state;

struct note_form {
  struct props {
    // Whether the note was added: one that was not comes back in the input.
    callback<bool(std::string)> on_add;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  state<std::string> draft;

  quoll::async::task<> add(callback<bool(std::string)> on_add) {
    if (draft->empty()) {
      co_return;
    }

    // Out of the input as it leaves: what is typed meanwhile is the next note, and a second click sends nothing.
    auto const sent = *draft;

    draft.set({});

    bool const added = co_await on_add(sent);

    if (!added && draft->empty()) {
      draft = sent;
    }
  }

  element view(props const &given) {
    using namespace quoll::ui;

    auto const send = [this, &given] { return add(given.on_add); };

    auto const form = div({
        input() | bind(draft) | tw("input input-bordered flex-1") | placeholder("A new note"),
        button("Add") | tw("btn btn-primary") | on_click(send),
    });

    return form | tw("flex gap-2");
  }
};

} // namespace notes_web

#endif // QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTE_FORM_HPP
