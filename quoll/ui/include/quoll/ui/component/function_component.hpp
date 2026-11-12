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

#ifndef QUOLL_UI_COMPONENT_FUNCTION_COMPONENT_HPP
#define QUOLL_UI_COMPONENT_FUNCTION_COMPONENT_HPP

#include <quoll/ui/view/element.hpp>

#include <type_traits>

namespace quoll::ui {

namespace detail {

// What a function component is: a function giving its view, of its props, taken by value or by const reference, or of
// nothing; the props it takes.
template <class F>
struct function_component_traits {
  static constexpr bool valid = false;
  using props = void;
};

template <>
struct function_component_traits<element (*)()> {
  static constexpr bool valid = true;
  using props = void;
};

template <class P>
struct function_component_traits<element (*)(P)> {
  static constexpr bool valid =
      std::is_same_v<P, std::remove_cvref_t<P>> || std::is_same_v<P, std::remove_cvref_t<P> const &>;
  using props = std::remove_cvref_t<P>;
};

template <>
struct function_component_traits<element (*)() noexcept> : function_component_traits<element (*)()> {};

template <class P>
struct function_component_traits<element (*)(P) noexcept> : function_component_traits<element (*)(P)> {};

[[nodiscard]] inline element function_component_model() { return {}; }

} // namespace detail

/**
 * @brief A function giving a view, `element f(props const &)`, `element f(props)` or `element f()`: a component whose
 * state is the `use_` hooks its view calls.
 */
template <auto F>
concept FunctionComponent = detail::function_component_traits<decltype(F)>::valid;

static_assert(FunctionComponent<&detail::function_component_model>);

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_FUNCTION_COMPONENT_HPP
