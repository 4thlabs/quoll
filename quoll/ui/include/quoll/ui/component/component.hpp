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

#ifndef QUOLL_UI_COMPONENT_COMPONENT_HPP
#define QUOLL_UI_COMPONENT_COMPONENT_HPP

#include <quoll/ui/component/component_view.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/component/component_source.hpp>
#include <quoll/ui/detail/component/hooks_table.hpp>
#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace quoll::ui {

namespace detail {

// Whether props refer to what they do not hold, a view or a pointer: each of several parameters, or the one.
template <class P>
inline constexpr bool refers_out = holds_view<P> || std::is_pointer_v<P> || std::is_member_pointer_v<P>;

template <class P>
inline constexpr bool holds_view_props = refers_out<P>;

template <>
inline constexpr bool holds_view_props<void> = false;

template <class... A>
inline constexpr bool holds_view_props<parameters<A...>> = (refers_out<A> || ...);

// The table of components made of F, once it is checked: what a component is made of says what is wrong with it.
template <class Props, class F>
[[nodiscard]] consteval component_table const *component_table_for() {
  using traits = view_traits<F>;

  static_assert(
      traits::valid || traits::not_by_const_reference,
      "quoll::ui: a component is made of a lambda or an empty struct whose call operator is `element (props const &)`, "
      "`element (a const &, b const &...)` or `element ()`"
  );
  static_assert(
      !traits::not_by_const_reference,
      "quoll::ui: a component takes its props by const reference, `element (props const &)`, which "
      "what its view writes may capture"
  );
  static_assert(
      std::is_empty_v<F> && std::default_initializable<F>,
      "quoll::ui: a component captures nothing, made for each view: its state is its use_ hooks"
  );
  static_assert(
      std::same_as<typename traits::props, Props>, "quoll::ui: a component<Props> is made of a view taking Props"
  );
  static_assert(
      !holds_view_props<typename traits::props>,
      "quoll::ui: a component keeps its props, which outlive its parent's view: none is a view, as a std::string_view, "
      "a std::span or a pointer, but owns what it holds, a std::string"
  );

  if constexpr (ComponentView<F> && std::same_as<typename traits::props, Props>) {
    return &view_table_of<F>;
  } else {
    return nullptr;
  }
}

template <class Props>
class component_base {
public:
  // Its identity: what the app keeps of a component placed, found again at the same place by it.
  [[nodiscard]] constexpr component_table const &table() const noexcept { return *table_; }

protected:
  template <class F>
  consteval explicit component_base(F) noexcept : table_(component_table_for<Props, F>()) {}

private:
  component_table const *table_;
};

} // namespace detail

/**
 * @brief A component: a piece of interface with its state, its view a function of its props, placed by calling it in
 * the view of another, as React's.
 *
 * @code
 * // Its props, its parameters: a component<products::product>.
 * inline constexpr component product_detail = [](products::product const &item) {
 *   auto &form = use_form(item);
 *
 *   return section({h2(text(item.name)), input() | bind(form, &products::product::name)});
 * };
 *
 * // Several, placed in order; or one struct, whose fields are named as it is placed.
 * inline constexpr component product_row = [](products::product const &item, bool const &selected) { ... };
 * inline constexpr component chart = [](chart_props const &given) { ... };
 *
 * // In the view of another:
 * return div({product_detail(chosen), product_row(item, true), chart({.title = "Sales"})});
 * @endcode
 *
 * Made of a lambda capturing nothing, or an empty struct, whose call operator gives the view, each of its parameters by
 * const reference: `element (props const &)`, `element (a const &, b const &...)`, or `element ()` for one with none,
 * a `component<>`. Its props are deduced from them, `component name = ...`, or written, `component<props>` or
 * `component<parameters<a, b>>` for several, as an `extern` declaration needs. They are kept with the component, past
 * its parent's view: a view, as a `std::string_view`, does not compile. Its state is the `use_` hooks its view calls,
 * found again at each view by where they are called. `inline constexpr` in a header, or declared in a header as `extern
 * component<props> const name;` and defined in a source of its own as `constexpr component<props> name = ...`, which a
 * change to it alone compiles again.
 *
 * Placed, the app keeps its instance, its hooks with it, from one view of the parent to the next, while it stays at the
 * same place, and gives it its props, unless they compare equal to the last ones and none of its states changed: then
 * its last view is given again, with the last props. Its place is what it is made of and its key among the components
 * of the parent's view, or its rank there without a key: `| key(...)` keeps a component of a list with its own item as
 * the list changes, and is unique among those of the same component in that view. Decorated only with a key: what it
 * shows is decorated in its own view. Once its parent's view no longer has it, it goes: its effects and actions are
 * asked to stop, and it goes once they have ended.
 */
template <class Props = void>
class component : public detail::component_base<Props> {
public:
  using props_type = Props;

  /** @brief Made of `view`, at compile time. */
  template <class F>
    requires detail::ComponentSource<F>
  // NOLINTNEXTLINE(google-explicit-constructor): it stands for its function, which a lambda makes.
  consteval component(F view) noexcept : detail::component_base<Props>(view) {}

  /** @brief Placed in a view, given `given`. */
  [[nodiscard]] element operator()(Props given) const { return detail::hooks_element(this->table(), std::move(given)); }
};

/** @brief A component with no props. */
template <>
class component<void> : public detail::component_base<void> {
public:
  using props_type = void;

  /** @brief Made of `view`, at compile time. */
  template <class F>
    requires detail::ComponentSource<F>
  // NOLINTNEXTLINE(google-explicit-constructor): it stands for its function, which a lambda makes.
  consteval component(F view) noexcept : detail::component_base<void>(view) {}

  /** @brief Placed in a view. */
  [[nodiscard]] element operator()() const { return detail::hooks_element(this->table()); }
};

/**
 * @brief A component of several props, its view taking each, placed with them in order: `product_row(item, true)`.
 */
template <class... A>
class component<parameters<A...>> : public detail::component_base<parameters<A...>> {
public:
  using props_type = parameters<A...>;

  /** @brief Made of `view`, at compile time. */
  template <class F>
    requires detail::ComponentSource<F>
  // NOLINTNEXTLINE(google-explicit-constructor): it stands for its function, which a lambda makes.
  consteval component(F view) noexcept : detail::component_base<props_type>(view) {}

  /** @brief Placed in a view, given its props in the order of its parameters, kept with it. */
  [[nodiscard]] element operator()(A... given) const {
    return detail::hooks_element(this->table(), props_type{{std::move(given)...}});
  }
};

// Its props, as its view takes them: `component badge = [](std::string const &label) {...}` is a
// component<std::string>, one of none a component<>, one of several a component<parameters<...>>.
template <class F>
component(F) -> component<typename detail::view_traits<F>::props>;

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_COMPONENT_HPP
