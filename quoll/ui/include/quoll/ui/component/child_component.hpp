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

#ifndef QUOLL_UI_COMPONENT_CHILD_COMPONENT_HPP
#define QUOLL_UI_COMPONENT_CHILD_COMPONENT_HPP

#include <quoll/ui/component/callable_component.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/component/component_with_props.hpp>
#include <quoll/ui/component/function_component.hpp>
#include <quoll/ui/detail/component/component_table.hpp>
#include <quoll/ui/detail/component/hooks_table.hpp>
#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief A component of the view, given `given` as its props: the app keeps its instance, its states with it, from one
 * view of the parent to the next, while it stays at the same place, and gives it these props, unless they compare equal
 * to the last ones and none of its states changed: then its last view is given again, with the last props.
 *
 * @code
 * auto const row = [](notes::note const &item) {
 *   return component<note_row>({.item = item}) | key(item.id);
 * };
 *
 * return ul({list(items, row)});
 * @endcode
 *
 * Its place is its type and key among the components of the parent's view, or its type and rank there without a key:
 * `| key(...)` keeps a component of a list with its own item as the list changes, and is unique among the components of
 * its type in that view. Decorated only with a key: what it shows is decorated in its own view. Once its parent's view
 * no longer has it, it is unmounted: its tasks are asked to stop, its unmounted() runs, and it goes once they have
 * ended. Its mounted() runs once it first shows.
 */
template <ComponentWithProps C>
[[nodiscard]] element component(typename C::props given) {
  static_assert(std::is_aggregate_v<C>, "quoll::ui: a component is an aggregate: public members and no constructor");

  element made;

  made.type = element::kind::component;
  made.component = std::make_shared<detail::component_spec const>(
      detail::component_spec{&detail::table_of<C>, std::make_shared<typename C::props const>(std::move(given))}
  );
  return made;
}

/** @brief A component of the view with no props, as component<C>(props) has: one with props takes them. */
template <Component C>
  requires(!ComponentWithProps<C>)
[[nodiscard]] element component() {
  static_assert(std::is_aggregate_v<C>, "quoll::ui: a component is an aggregate: public members and no constructor");

  element made;

  made.type = element::kind::component;
  made.component =
      std::make_shared<detail::component_spec const>(detail::component_spec{&detail::table_of<C>, nullptr});
  return made;
}

/**
 * @brief A function component of the view, given its props: `component<product_view>({.id = id})`, for
 * `element product_view(product_view_props const &given)`. Its states, effects and contexts are what its view's use_
 * calls make, found again at each view by where they are called. Placed, kept and gone as a struct component is.
 */
template <auto F>
  requires FunctionComponent<F> && (!std::is_void_v<typename detail::function_component_traits<decltype(F)>::props>)
[[nodiscard]] element component(typename detail::function_component_traits<decltype(F)>::props given) {
  return detail::hooks_element(detail::function_table_of<F>, std::move(given));
}

/** @brief A function component of the view with no props. */
template <auto F>
  requires FunctionComponent<F> && std::is_void_v<typename detail::function_component_traits<decltype(F)>::props>
[[nodiscard]] element component() {
  return detail::hooks_element(detail::function_table_of<F>);
}

/** @brief A struct function component of the view, given its props: see CallableComponent. */
template <CallableComponent C>
  requires(!std::is_void_v<typename detail::callable_component_traits<C>::props>)
[[nodiscard]] element component(typename detail::callable_component_traits<C>::props given) {
  static_assert(
      std::is_empty_v<C> && std::default_initializable<C>,
      "quoll::ui: a struct function component holds no member, made for each view: its state is its use_ hooks"
  );

  return detail::hooks_element(detail::callable_table_of<C>, std::move(given));
}

/** @brief A struct function component of the view with no props. */
template <CallableComponent C>
  requires std::is_void_v<typename detail::callable_component_traits<C>::props>
[[nodiscard]] element component() {
  static_assert(
      std::is_empty_v<C> && std::default_initializable<C>,
      "quoll::ui: a struct function component holds no member, made for each view: its state is its use_ hooks"
  );

  return detail::hooks_element(detail::callable_table_of<C>);
}

/** @brief What is not a component, given as one: a function whose signature is not a view's says so. */
template <auto F>
  requires(!FunctionComponent<F>) && std::is_pointer_v<decltype(F)> &&
          std::is_function_v<std::remove_pointer_t<decltype(F)>>
element component(auto &&...) {
  static_assert(
      FunctionComponent<F>,
      "quoll::ui: a function component is `element f(props const &)`, `element f(props)` or `element f()`"
  );
  return {};
}

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_CHILD_COMPONENT_HPP
