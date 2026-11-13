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

#ifndef QUOLL_UI_COMPONENT_COMPONENT_VIEW_HPP
#define QUOLL_UI_COMPONENT_COMPONENT_VIEW_HPP

#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <tuple>
#include <type_traits>

namespace quoll::ui {

/**
 * @brief The props of a component whose view takes several parameters: their values, in order, compared together when
 * each has its `==`. Its type, as an `extern` declaration writes it: `extern component<parameters<note, bool>> const
 * note_row;`.
 */
template <class... A>
struct parameters {
  std::tuple<A...> values;

  bool operator==(parameters const &) const
    requires(std::equality_comparable<A> && ...)
  = default;
};

namespace detail {

// The props of a view taking `A...`: none, the one it takes, or its parameters together.
template <class... A>
struct props_of_parameters {
  using type = parameters<A...>;
};

template <>
struct props_of_parameters<> {
  using type = void;
};

template <class A>
struct props_of_parameters<A> {
  using type = A;
};

// Whether a view takes `A` by const reference, as its props: not an array or a function, which a placement would take
// as a pointer.
template <class A>
inline constexpr bool by_const_reference =
    std::is_lvalue_reference_v<A> && std::is_const_v<std::remove_reference_t<A>> &&
    !std::is_array_v<std::remove_cvref_t<A>> && !std::is_function_v<std::remove_cvref_t<A>>;

// What a call operator takes: its props by const reference, each of its parameters, or nothing; one taken otherwise,
// by value or by a reference not const, is told apart, to say so.
template <class S>
struct call_operator_traits {
  static constexpr bool valid = false;
  static constexpr bool not_by_const_reference = false;
  using props = void;
};

template <class C, class... A>
struct call_operator_traits<element (C::*)(A...)> {
  static constexpr bool valid = (by_const_reference<A> && ...);
  static constexpr bool not_by_const_reference = !valid;
  using props = typename props_of_parameters<std::remove_cvref_t<A>...>::type;
};

template <class C, class... A>
struct call_operator_traits<element (C::*)(A...) const> : call_operator_traits<element (C::*)(A...)> {};

template <class C, class... A>
struct call_operator_traits<element (C::*)(A...) noexcept> : call_operator_traits<element (C::*)(A...)> {};

template <class C, class... A>
struct call_operator_traits<element (C::*)(A...) const noexcept> : call_operator_traits<element (C::*)(A...)> {};

template <class F>
struct view_traits : call_operator_traits<void> {};

template <class F>
  requires std::is_class_v<F> && requires { &F::operator(); }
struct view_traits<F> : call_operator_traits<decltype(&F::operator())> {};

struct component_view_model {
  element operator()() const { return {}; }
};

} // namespace detail

/**
 * @brief What a component is made of: a lambda capturing nothing, or an empty struct, whose call operator gives the
 * view, `element (props const &)`, `element (a const &, b const &...)`, or `element ()`. Made for each view, it holds
 * nothing: what lasts is its `use_` hooks. Its props by const reference, which what its view writes may capture, kept
 * with it.
 */
template <class F>
concept ComponentView = detail::view_traits<F>::valid && std::is_empty_v<F> && std::default_initializable<F>;

static_assert(ComponentView<detail::component_view_model>);

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_COMPONENT_VIEW_HPP
