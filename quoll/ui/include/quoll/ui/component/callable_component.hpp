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

#ifndef QUOLL_UI_COMPONENT_CALLABLE_COMPONENT_HPP
#define QUOLL_UI_COMPONENT_CALLABLE_COMPONENT_HPP

#include <quoll/ui/view/element.hpp>

#include <type_traits>

namespace quoll::ui {

namespace detail {

// The operator() of a struct function component: of its props, taken by value or by const reference, or of nothing,
// const or not, noexcept or not.
template <class S>
struct call_operator_traits {
  static constexpr bool valid = false;
  using props = void;
};

template <class C>
struct call_operator_traits<element (C::*)()> {
  static constexpr bool valid = true;
  using props = void;
};

template <class C, class P>
struct call_operator_traits<element (C::*)(P)> {
  static constexpr bool valid =
      std::is_same_v<P, std::remove_cvref_t<P>> || std::is_same_v<P, std::remove_cvref_t<P> const &>;
  using props = std::remove_cvref_t<P>;
};

template <class C>
struct call_operator_traits<element (C::*)() const> : call_operator_traits<element (C::*)()> {};

template <class C, class P>
struct call_operator_traits<element (C::*)(P) const> : call_operator_traits<element (C::*)(P)> {};

template <class C>
struct call_operator_traits<element (C::*)() noexcept> : call_operator_traits<element (C::*)()> {};

template <class C, class P>
struct call_operator_traits<element (C::*)(P) noexcept> : call_operator_traits<element (C::*)(P)> {};

template <class C>
struct call_operator_traits<element (C::*)() const noexcept> : call_operator_traits<element (C::*)()> {};

template <class C, class P>
struct call_operator_traits<element (C::*)(P) const noexcept> : call_operator_traits<element (C::*)(P)> {};

// A struct whose operator() gives the view; the props it takes.
template <class C>
struct callable_component_traits {
  static constexpr bool valid = false;
  using props = void;
};

template <class C>
  requires std::is_class_v<C> && requires { &C::operator(); }
struct callable_component_traits<C> : call_operator_traits<decltype(&C::operator())> {};

struct callable_component_model {
  element operator()() const { return {}; }
};

} // namespace detail

/**
 * @brief A struct whose `element operator()(props const &)`, or of nothing, gives the view: a function component that
 * keeps its props and helpers together. Made for each view, it holds no member: what lasts is its `use_` hooks.
 */
template <class C>
concept CallableComponent = detail::callable_component_traits<C>::valid;

static_assert(CallableComponent<detail::callable_component_model>);

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_CALLABLE_COMPONENT_HPP
