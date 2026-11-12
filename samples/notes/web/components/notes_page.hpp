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

// The page of the notes: the list, the form, and what the server answered last. Its calls to the server are in
// notes_client.cpp, so that a change here does not compile REST and JSON again.

#ifndef QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTES_PAGE_HPP
#define QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTES_PAGE_HPP

#include "note.hpp"
#include "note_form.hpp"
#include "note_row.hpp"
#include "notes_client.hpp"

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace notes_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::element;
using quoll::ui::state;

struct notes_page {
  state<std::vector<notes::note>> items;
  state<std::string> problem;

  task<> mounted() {
    try {
      items = co_await notes_client::list();
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  task<bool> add(std::string text) {
    try {
      auto const created = co_await notes_client::create(std::move(text));

      items.update([&created](auto &all) { all.push_back(created); });
      problem.set({});
      co_return true;
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
      co_return false;
    }
  }

  task<> remove(std::int64_t id) {
    try {
      co_await notes_client::remove(id);

      auto const removed = [id](notes::note const &item) { return item.id == id; };

      items.update([&removed](auto &all) { std::erase_if(all, removed); });
      problem.set({});
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  element view() {
    using namespace quoll::ui;

    auto const on_add = [this](std::string text) { return add(std::move(text)); };

    // One callback, which every row shares.
    callback<void(std::int64_t)> const on_remove = [this](std::int64_t id) { return remove(id); };

    auto const row = [&on_remove](notes::note const &item) {
      return component<note_row>({.item = item, .on_remove = on_remove}) | key(item.id);
    };

    auto const alert = [this] { return p(*problem) | tw("alert alert-error") | role("alert"); };

    auto const page = main_({
        h1("Notes") | tw("text-4xl font-bold"),
        component<note_form>({.on_add = on_add}),
        when(!problem->empty(), alert),
        ul({list(items, row)}) | tw("flex flex-col gap-2"),
    });

    return page | tw("mx-auto flex max-w-xl flex-col gap-6 p-8");
  }
};

} // namespace notes_web

#endif // QUOLL_SAMPLES_NOTES_WEB_COMPONENTS_NOTES_PAGE_HPP
