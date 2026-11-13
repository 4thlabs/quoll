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

#ifndef QUOLL_UI_DETAIL_COMPONENT_COMPONENT_TABLE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_COMPONENT_TABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/component/held_component.hpp>
#include <quoll/ui/detail/context/context_entry.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/view/element.hpp>

#include <memory>

namespace quoll::ui::detail {

// A component held without its type: its hook store, what to make it with, link its hooks to, find its contexts with,
// and view it with its props. One for each lambda or struct a component is made of, told apart by `type`: its address
// could be another's too, as a linker may fold identical constants.
struct component_table {
  async::detail::type_id type;
  // Made with the props of its first placing, which a component written as a struct reads as it is made.
  held_component (*make)(void const *props);
  void (*link)(void *component, state_link to);
  // The context of a type it provides to those of its view, none for one it does not.
  context_entry *(*provides)(void *component, async::detail::type_id type);
  // Finds the contexts it uses, marks what it sees of them as its view runs, keeps it once the view shows, and tells
  // whether one it read changed since.
  void (*bind)(void *component, context_finder const &finder);
  void (*mark_contexts)(void *component);
  void (*commit_contexts)(void *component);
  bool (*contexts_changed)(void *component);
  // After each of its views: its effects whose dependency changed start.
  void (*effects)(void *component, effect_starter const &start);
  element (*view)(void *component, void const *props);
  // Whether two props give the same view, for one with props comparable with ==, and always for one without props:
  // none for props that are not comparable, so that its view runs again on each of its parent's.
  bool (*same_props)(void const *left, void const *right);
};

// What a parent's view gives a component of its own: which, and its props, shared as an element is copied.
struct component_spec {
  component_table const *table;
  std::shared_ptr<void const> props;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_COMPONENT_TABLE_HPP
