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

#ifndef QUOLL_UI_COMPONENT_TAG_HPP
#define QUOLL_UI_COMPONENT_TAG_HPP

#include <quoll/ui/component/component.hpp>
#include <quoll/ui/detail/component/hooks_table.hpp>
#include <quoll/ui/detail/component/struct_table.hpp>
#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief What places a component written as a struct, as a tag places an element: a variable of the struct's name,
 * declared after it.
 *
 * @code
 * struct product_view : component<product_view> {
 *   struct props {
 *     products::product item;
 *
 *     bool operator==(props const &) const = default;
 *   };
 *
 *   // State
 *   async_state<products::monthly_sales> sales;
 *
 *   // Handlers
 *   void retry() { ... }
 *
 *   // View
 *   element view() { return div({p(given->item.name), button("Try again") | on_click([this] { retry(); })}); }
 * };
 *
 * inline constexpr tag<product_view> product_view;
 *
 * // In the view of another, as any component:
 * product_view({.item = chosen}) | key(chosen.id)
 * @endcode
 *
 * The struct derives from `component<name>`, its props, if it has some, are `struct props` inside it, read through
 * `given->`, and its view is `element view()`. The app makes the struct as it first places it, keeps it while it stays
 * at that place, gives it its props anew at each view of its parent, and destroys it as it goes, as any component. Its
 * members are made once: its states, effects, forms and the contexts it uses as `shared<T>` join it as they are made,
 * with no reflection, and its handlers are member functions, capturing `this`, which lives as long as the component.
 * Its view may still call `use_context<T>()` and `provide_context<T>()`. Routes, links and the root take the variable,
 * as they take a component made of a lambda.
 */
template <class C>
class tag : public detail::component_base<detail::struct_props_t<C>> {
public:
  using props_type = detail::struct_props_t<C>;

  constexpr tag() noexcept : detail::component_base<props_type>(detail::struct_table_of<C>) {
    static_assert(
        std::derived_from<C, component<C>>, "quoll::ui: a component written as a struct derives from component<name>"
    );
    static_assert(
        requires(C &held) {
          { held.view() } -> std::same_as<element>;
        }, "quoll::ui: a component written as a struct has its view, `element view()`"
    );
    static_assert(
        !detail::holds_view_props<props_type>, "quoll::ui: a component keeps its props, which outlive its parent's "
                                               "view: none is a view, as a std::string_view, "
                                               "a std::span or a pointer, but owns what it holds, a std::string"
    );
  }

  /** @brief Placed in a view, given its props. */
  template <class P = props_type>
    requires(!std::is_void_v<P>)
  [[nodiscard]] element operator()(std::type_identity_t<P> given) const {
    return detail::hooks_element(this->table(), std::move(given));
  }

  /** @brief Placed in a view, for one with no props. */
  [[nodiscard]] element operator()() const
    requires std::is_void_v<props_type>
  {
    return detail::hooks_element(this->table());
  }
};

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_TAG_HPP
