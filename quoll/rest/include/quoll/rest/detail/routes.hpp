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

#ifndef QUOLL_REST_DETAIL_ROUTES_HPP
#define QUOLL_REST_DETAIL_ROUTES_HPP

#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/category.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/route.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/endpoint.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <concepts>
#include <type_traits>

// How api<> collects the routes of its members: endpoints, nested apis and with<>.
namespace quoll::rest::detail {

template <class E>
struct endpoint_traits;

template <fixed_string Name, http::verb Method, fixed_string Path, class... Parts>
struct endpoint_traits<endpoint<Name, Method, Path, Parts...>> {
  static constexpr auto path = Path;
};

template <class T>
struct is_part : std::bool_constant<Part<T>> {};

template <class T>
struct is_member : std::bool_constant<!Part<T>> {};

// Moves routes under Prefix, and gives them the Inherited parts after their own.
template <fixed_string Prefix, class Inherited, class Routes>
struct rebase;

template <fixed_string Prefix, class Inherited, class... Routes>
struct rebase<Prefix, Inherited, type_list<Routes...>> {
  using type = type_list<route<typename Routes::endpoint, join_paths<Prefix, Routes::path_value>(),
                               concat_t<typename Routes::inherited, Inherited>>...>;
};

template <fixed_string Prefix, class Member>
struct routes_of;

template <fixed_string Prefix, Endpoint E>
struct routes_of<Prefix, E> {
  using type = type_list<route<E, join_paths<Prefix, endpoint_traits<E>::path>(), type_list<>>>;
};

template <fixed_string Prefix, Api A>
struct routes_of<Prefix, A> {
  using type = typename rebase<Prefix, type_list<>, typename A::routes>::type;
};

template <class Members>
struct member_routes;

template <class... Members>
struct member_routes<type_list<Members...>> {
  using type = concat_t<type_list<>, typename routes_of<"", Members>::type...>;
};

template <fixed_string Prefix, With W>
struct routes_of<Prefix, W> {
  using type = typename rebase<Prefix, filter_t<is_part, typename W::members>,
                               typename member_routes<filter_t<is_member, typename W::members>>::type>::type;
};

template <class E, class Routes>
struct matching_routes;

template <class E, class... Routes>
struct matching_routes<E, type_list<Routes...>> {
  using type =
      concat_t<type_list<>,
               std::conditional_t<std::same_as<typename Routes::endpoint, E>, type_list<Routes>, type_list<>>...>;
};

template <class E, class Matches>
struct single_route {
  static_assert(Matches::size != 0, "quoll::rest::api: the endpoint is not in this api");
  static_assert(Matches::size < 2, "quoll::rest::api: the endpoint is in this api more than once");
};

template <class E, class Route>
struct single_route<E, type_list<Route>> {
  using type = Route;
};

template <class E, class Routes>
using find_route_t = typename single_route<E, typename matching_routes<E, Routes>::type>::type;

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_ROUTES_HPP
