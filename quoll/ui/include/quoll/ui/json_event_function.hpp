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

#ifndef QUOLL_UI_JSON_EVENT_FUNCTION_HPP
#define QUOLL_UI_JSON_EVENT_FUNCTION_HPP

#include <quoll/ui/event_function.hpp>

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>

namespace quoll::ui {

namespace detail {

// The one parameter of a function, a lambda that is not generic or a pointer: what its event's JSON is read into.
template <class F>
struct one_parameter {};

template <class F>
  requires requires { &F::operator(); }
struct one_parameter<F> : one_parameter<decltype(&F::operator())> {};

template <class C, class R, class A>
struct one_parameter<R (C::*)(A) const> {
  using type = std::remove_cvref_t<A>;
};

template <class C, class R, class A>
struct one_parameter<R (C::*)(A)> {
  using type = std::remove_cvref_t<A>;
};

template <class R, class A>
struct one_parameter<R (*)(A)> {
  using type = std::remove_cvref_t<A>;
};

template <class F>
using one_parameter_t = typename one_parameter<std::remove_cvref_t<F>>::type;

} // namespace detail

/**
 * @brief What can run on an event whose value is JSON, as one a widget emits: a function of one parameter, neither a
 * string nor a view of one, which the JSON is read into, copied for each event, and returning nothing or something to
 * await.
 */
template <class F>
concept JsonEventFunction =
    std::copy_constructible<F> && requires { typename detail::one_parameter_t<F>; } &&
    !std::same_as<detail::one_parameter_t<F>, std::string> &&
    !std::same_as<detail::one_parameter_t<F>, std::string_view> &&
    std::default_initializable<detail::one_parameter_t<F>> && detail::EventCallable<F, detail::one_parameter_t<F> &>;

} // namespace quoll::ui

#endif // QUOLL_UI_JSON_EVENT_FUNCTION_HPP
