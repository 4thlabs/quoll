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

#ifndef QUOLL_UI_DETAIL_CONTEXT_CONTEXT_ENTRY_HPP
#define QUOLL_UI_DETAIL_CONTEXT_CONTEXT_ENTRY_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/detail/component/held_component.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/detail/view/view_context.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

class context_entry;

// A context found, and what holds what provides it, a component above, for as long as the one using it lives: none for
// the app's, which outlives its components.
struct found_context {
  context_entry *entry = nullptr;
  std::shared_ptr<void const> keeper{};
};

// Where a component finds its contexts, by type: those the components above it provide, nearest first, then those the
// app's own component provides, then the app's; none for a view written once (to_html).
struct context_finder {
  std::shared_ptr<app_link> app{};
  void const *owner = nullptr;
  context_entry *(*find)(void const *owner, async::detail::type_id type) noexcept = nullptr;
  // The component placing the one it finds for, from which those above are walked: none for the app's own.
  std::shared_ptr<void const> from{};
  found_context (*find_above)(std::shared_ptr<void const> const &from, async::detail::type_id type) noexcept = nullptr;

  // The context of that type, or none.
  [[nodiscard]] found_context operator()(async::detail::type_id type) const noexcept {
    if (from && find_above != nullptr) {
      if (auto found = find_above(from, type); found.entry != nullptr) {
        return found;
      }
    }

    return {.entry = find == nullptr ? nullptr : find(owner, type), .keeper = nullptr};
  }
};

// A context held without its type: what to make it with, link its states to, start its effects with, and find the
// contexts it uses itself. One per type, told apart by `type`.
struct context_table {
  async::detail::type_id type;
  held_component (*make)();
  void (*link)(void *context, state_link to);
  // None for a context without effects.
  void (*effects)(void *context, effect_starter const &start);
  // None for a context using none: whether those it uses are there, before it is made, then finds them, then counts
  // their changes.
  bool (*finds)(context_finder const &finder);
  void (*bind)(void *context, context_finder const &finder);
  std::uint64_t (*contexts_version)(void *context);
};

// A context of an app: the object, and how many times its states changed, which a component using it reads, as its
// view runs again once one did. Neither copied nor moved: its states and the components using it refer to it.
//
// Thread safety: none, as its app.
class context_entry {
public:
  // Made, its states telling `app` of their changes, which the app hears only while it is made and not going: the
  // object as `table` makes it, or `object`.
  context_entry(context_table const &table, state_link app);
  context_entry(context_table const &table, state_link app, held_component object);

  context_entry(context_entry const &) = delete;
  context_entry(context_entry &&) = delete;
  context_entry &operator=(context_entry const &) = delete;
  context_entry &operator=(context_entry &&) = delete;

  ~context_entry();

  [[nodiscard]] context_table const &table() const noexcept { return *table_; }

  // Its changes are told to `app` from now on: a context a component provides tells its component.
  void link(state_link app) noexcept { app_ = app; }

  [[nodiscard]] void *object() const noexcept { return object_.get(); }

  // How many times its states, and those of the contexts it uses, changed.
  [[nodiscard]] std::uint64_t version() const noexcept;

private:
  static void changed_in(void *self);

  context_table const *table_;
  state_link app_{};
  std::uint64_t version_ = 0;
  // Last, as it refers to what is above.
  held_component object_;
};

// The contexts of an app, destroyed the last made first, as one may use those made before it, in its destructor too:
// whatever destroys the list, the app, or its constructor failing past some of them.
class context_list {
public:
  context_list() = default;

  context_list(context_list const &) = delete;
  context_list(context_list &&) = delete;
  context_list &operator=(context_list const &) = delete;
  context_list &operator=(context_list &&) = delete;

  ~context_list();

  void push_back(std::unique_ptr<context_entry> entry);

  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }

  [[nodiscard]] auto end() const noexcept { return entries_.end(); }

private:
  std::vector<std::unique_ptr<context_entry>> entries_;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CONTEXT_CONTEXT_ENTRY_HPP
