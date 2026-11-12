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

#ifndef QUOLL_UI_DETAIL_COMPONENT_TABLE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_TABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/component_with_props.hpp>
#include <quoll/ui/detail/bind_contexts.hpp>
#include <quoll/ui/detail/effect_starter.hpp>
#include <quoll/ui/detail/held_component.hpp>
#include <quoll/ui/detail/link_states.hpp>
#include <quoll/ui/detail/run_effects.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/mounted_component.hpp>
#include <quoll/ui/state.hpp>
#include <quoll/ui/unmounted_component.hpp>

#include <concepts>
#include <cstdint>
#include <memory>

namespace quoll::ui::detail {

// A component held without its type: what to make it with, link its states to, find its contexts with, view it with its
// props, and run as it comes and goes. One per type, told apart by `type`: its address could be another type's too, as
// a linker may fold identical constants.
struct component_table {
  async::detail::type_id type;
  held_component (*make)();
  void (*link)(void *component, state_link to);
  // None for a component using no context: whether those it uses are there, before it is made, then finds them, marks
  // what it sees of them as its view runs, keeps it once the view shows, and tells whether one it read changed since.
  bool (*finds)(context_finder const &finder);
  // None for a component providing no context: the one of a type it provides, to those of its view.
  context_entry *(*provides)(void *component, async::detail::type_id type);
  void (*bind)(void *component, context_finder const &finder);
  void (*mark_contexts)(void *component);
  void (*commit_contexts)(void *component);
  bool (*contexts_changed)(void *component);
  // After each of its views: its effects whose dependency changed start.
  void (*effects)(void *component, effect_starter const &start);
  element (*view)(void *component, void const *props);
  // None for a component that has none.
  async::task<> (*mounted)(void *component);
  void (*unmounted)(void *component);
  // Whether two props give the same view, for one with props comparable with ==, and always for one without props:
  // none for props that are not comparable, so that its view runs again on each of its parent's.
  bool (*same_props)(void const *left, void const *right);
};

// What a parent's view gives a component of its own: which, and its props, shared as an element is copied.
struct component_spec {
  component_table const *table;
  std::shared_ptr<void const> props;
};

template <class C>
[[nodiscard]] consteval auto mounted_of() -> async::task<> (*)(void *) {
  if constexpr (MountedComponent<C>) {
    return [](void *component) { return static_cast<C *>(component)->mounted(); };
  } else {
    return nullptr;
  }
}

template <class C>
[[nodiscard]] consteval auto unmounted_of() -> void (*)(void *) {
  if constexpr (UnmountedComponent<C>) {
    return [](void *component) { static_cast<C *>(component)->unmounted(); };
  } else {
    return nullptr;
  }
}

template <class C>
[[nodiscard]] consteval auto same_props_of() -> bool (*)(void const *, void const *) {
  if constexpr (!ComponentWithProps<C>) {
    return [](void const *, void const *) { return true; };
  } else if constexpr (std::equality_comparable<typename C::props>) {
    return [](void const *left, void const *right) {
      return *static_cast<typename C::props const *>(left) == *static_cast<typename C::props const *>(right);
    };
  } else {
    return nullptr;
  }
}

template <class C>
inline constexpr component_table table_of{
    .type = async::detail::type_id_of<C>(),
    .make = &make_held<C>,
    .link = [](void *component, state_link to) { link_states(*static_cast<C *>(component), to); },
    .finds = finds_of<C>(),
    .provides = provides_of<C>(),
    .bind = binds_of<C>(),
    .mark_contexts = marks_of<C>(),
    .commit_contexts = commits_of<C>(),
    .contexts_changed = changes_of<C>(),
    .effects = effects_of<C>(),
    .view = [](void *component, [[maybe_unused]] void const *props) -> element {
      if constexpr (ComponentWithProps<C>) {
        return static_cast<C *>(component)->view(*static_cast<typename C::props const *>(props));
      } else {
        return static_cast<C *>(component)->view();
      }
    },
    .mounted = mounted_of<C>(),
    .unmounted = unmounted_of<C>(),
    .same_props = same_props_of<C>(),
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_TABLE_HPP
