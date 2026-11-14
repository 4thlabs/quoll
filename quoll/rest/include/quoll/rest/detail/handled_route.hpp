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

#ifndef QUOLL_REST_DETAIL_HANDLED_ROUTE_HPP
#define QUOLL_REST_DETAIL_HANDLED_ROUTE_HPP

#include <quoll/rest/detail/operation_concept.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <cstddef>
#include <type_traits>

namespace quoll::rest {

template <class... Handlers>
class service;

namespace detail {

template <class Call>
struct call_first_parameter {
  using type = void;
};

template <class C, class R, class First, class... Rest>
struct call_first_parameter<R (C::*)(First, Rest...)> {
  using type = std::remove_cvref_t<First>;
};

template <class C, class R, class First, class... Rest>
struct call_first_parameter<R (C::*)(First, Rest...) const> {
  using type = std::remove_cvref_t<First>;
};

template <class C, class R, class First, class... Rest>
struct call_first_parameter<R (C::*)(First, Rest...) noexcept> {
  using type = std::remove_cvref_t<First>;
};

template <class C, class R, class First, class... Rest>
struct call_first_parameter<R (C::*)(First, Rest...) const noexcept> {
  using type = std::remove_cvref_t<First>;
};

// The route a handler takes first, when it has one call operator, not a template: void otherwise, a struct of several
// or a generic lambda, which no check sees.
template <class Handler>
struct handled_route {
  using type = void;
};

template <class Handler>
  requires requires { &Handler::operator(); }
struct handled_route<Handler> {
  using first = typename call_first_parameter<decltype(&Handler::operator())>::type;
  using type = std::conditional_t<Operation<first>, first, void>;
};

// The routes the handlers of a mount() argument take: one for a lambda, those of its handlers for a rest::service.
template <class Argument>
struct handled_routes {
  using type = type_list<typename handled_route<Argument>::type>;
};

template <class... Handlers>
struct handled_routes<service<Handlers...>> {
  using type = type_list<typename handled_route<Handlers>::type...>;
};

template <class Route, class... Routes>
[[nodiscard]] consteval std::size_t count_route(type_list<Routes...>) {
  return (std::size_t{0} + ... + std::size_t{std::is_same_v<Route, Routes>});
}

// Whether no route is taken by two of these handlers.
template <class... Routes>
[[nodiscard]] consteval bool distinct_handled_routes(type_list<Routes...> all) {
  return ((std::is_void_v<Routes> || count_route<Routes>(all) == 1) && ...);
}

} // namespace detail

} // namespace quoll::rest

#endif // QUOLL_REST_DETAIL_HANDLED_ROUTE_HPP
