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
#include <type_traits>

namespace quoll::ui {

namespace detail {

// What a call operator takes: its props by const reference, or nothing; props by value are told apart, to say so.
template <class S>
struct call_operator_traits {
  static constexpr bool valid = false;
  static constexpr bool by_value = false;
  using props = void;
};

template <class C>
struct call_operator_traits<element (C::*)()> {
  static constexpr bool valid = true;
  static constexpr bool by_value = false;
  using props = void;
};

template <class C, class P>
struct call_operator_traits<element (C::*)(P const &)> {
  static constexpr bool valid = true;
  static constexpr bool by_value = false;
  using props = P;
};

template <class C, class P>
  requires(!std::is_reference_v<P>)
struct call_operator_traits<element (C::*)(P)> {
  static constexpr bool valid = false;
  static constexpr bool by_value = true;
  using props = P;
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
 * view, `element (props const &)` or `element ()`. Made for each view, it holds nothing: what lasts is its `use_`
 * hooks. Its props by const reference, which what its view writes may capture, kept with it.
 */
template <class F>
concept ComponentView = detail::view_traits<F>::valid && std::is_empty_v<F> && std::default_initializable<F>;

static_assert(ComponentView<detail::component_view_model>);

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_COMPONENT_VIEW_HPP
