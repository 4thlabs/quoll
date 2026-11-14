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

#ifndef QUOLL_REST_API_HPP
#define QUOLL_REST_API_HPP

#include <quoll/rest/detail/category.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/operation_concept.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/route.hpp>
#include <quoll/rest/detail/routes.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/security.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>

namespace quoll::rest {

namespace detail {

// What a group gives all its routes, after their own arguments.
template <class T>
struct is_group_option : std::false_type {};

template <class... Schemes>
struct is_group_option<security<Schemes...>> : std::true_type {};

template <fixed_string Name, class T>
struct is_group_option<header<Name, T>> : std::true_type {};

} // namespace detail

/**
 * @brief Routes and what they share, with no prefix: `rest::group<rest::security<rest::bearer_auth>, list, get>`. Its
 * members are routes, groups, and what it gives all its routes, `security<>` and `header<>`, in any order; what it
 * gives comes after a route's own arguments. Groups nest; only api<> has a prefix, so that a route's string says its
 * whole path below it.
 */
template <class... Members>
struct group {
  static_assert(
      ((detail::Operation<Members> || detail::With<Members> || detail::is_group_option<Members>::value) && ...),
      "quoll::rest::group: a member is a route, a group, a security<> or a header<>"
  );

  using contract_category = detail::with_category;
  using members = detail::type_list<Members...>;
};

/**
 * @brief The routes of an API, under `Prefix` (`"/api"`, a version, or none): routes and groups.
 *
 * The prefix has no parameter: one its routes share goes in each one's path. Checked at compile time: no two routes
 * share a method and a path, nor a name, and a route is in it once.
 */
template <fixed_string Prefix, class... Members>
struct api {
  static_assert(detail::checked_path<Prefix>::valid);
  static_assert(Prefix.view().empty() || Prefix.view().back() != '/', "quoll::rest::api: the prefix ends with '/'");
  static_assert(
      detail::checked_path<Prefix>::parsed.size == 0,
      "quoll::rest::api: the prefix has no parameter: one its routes share goes in each one's path"
  );
  static_assert(
      ((detail::Operation<Members> || detail::With<Members>) && ...), "quoll::rest::api: a member is a route or a group"
  );

  using contract_category = detail::api_category;
  using routes = detail::concat_t<detail::type_list<>, typename detail::routes_of<Prefix, Members>::type...>;

  static_assert(detail::distinct_names(routes{}), "quoll::rest::api: two routes have the same operation_id<>");
  static_assert(detail::distinct_operations(routes{}), "quoll::rest::api: two routes have the same method and path");

  /** @brief How this api serves `Route`: its full path, arguments, result and security schemes. */
  template <class Route>
  using route = detail::find_route_t<Route, routes>;

  static constexpr std::string_view prefix = Prefix.view();
};

} // namespace quoll::rest

#endif // QUOLL_REST_API_HPP
