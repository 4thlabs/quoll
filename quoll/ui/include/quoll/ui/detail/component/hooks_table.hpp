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

#ifndef QUOLL_UI_DETAIL_COMPONENT_HOOKS_TABLE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_HOOKS_TABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/component/component_view.hpp>
#include <quoll/ui/detail/component/component_table.hpp>
#include <quoll/ui/detail/component/hook_store.hpp>
#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

template <class P>
inline constexpr bool is_parameters = false;

template <class... A>
inline constexpr bool is_parameters<parameters<A...>> = true;

template <class F>
element view_component(void *store, [[maybe_unused]] void const *props) {
  using props_type = typename view_traits<F>::props;

  hook_scope const scope{*static_cast<hook_store *>(store)};
  // Made for its view: it holds nothing, so that an operator() that is not const may be called too.
  F viewing{};

  if constexpr (std::is_void_v<props_type>) {
    return viewing();
  } else if constexpr (is_parameters<props_type>) {
    return std::apply(viewing, static_cast<props_type const *>(props)->values);
  } else {
    return viewing(*static_cast<props_type const *>(props));
  }
}

template <class P>
bool same_hooks_props(void const *left, void const *right) {
  return *static_cast<P const *>(left) == *static_cast<P const *>(right);
}

template <class P>
[[nodiscard]] consteval auto same_hooks_props_of() -> bool (*)(void const *, void const *) {
  if constexpr (std::is_void_v<P>) {
    return &same_without_hooks_props;
  } else if constexpr (std::equality_comparable<P>) {
    return &same_hooks_props<P>;
  } else {
    return nullptr;
  }
}

// A table of a component whose state is its hooks: `type` tells it apart, `view` gives its view and `same_props`
// compares its props; the rest is the store of its hooks, compiled once.
[[nodiscard]] consteval component_table hooks_table(
    async::detail::type_id type, element (*view)(void *, void const *), bool (*same_props)(void const *, void const *)
) {
  return {
      .type = type,
      .make = &make_hook_store,
      .link = &link_hook_store,
      .provides = &hook_store_provides,
      .bind = &bind_hook_store,
      .mark_contexts = &mark_hook_store,
      .commit_contexts = &commit_hook_store,
      .contexts_changed = &hook_store_changed,
      .effects = &hook_store_effects,
      .view = view,
      .same_props = same_props,
  };
}

// One for each lambda or struct a component is made of: what tells two components apart.
template <class F>
inline constexpr component_table view_table_of = hooks_table(
    async::detail::type_id_of<F>(), &view_component<F>, same_hooks_props_of<typename view_traits<F>::props>()
);

template <class P>
[[nodiscard]] element hooks_element(component_table const &table, P given) {
  element made;

  made.type = element::kind::component;
  made.component =
      std::make_shared<component_spec const>(component_spec{&table, std::make_shared<P const>(std::move(given))});
  return made;
}

// An element of the component `table`, which takes no props.
[[nodiscard]] inline element hooks_element(component_table const &table) {
  element made;

  made.type = element::kind::component;
  made.component = std::make_shared<component_spec const>(component_spec{&table, nullptr});
  return made;
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_HOOKS_TABLE_HPP
