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

#include <concepts>
#include <string_view>
#include <type_traits>

namespace quoll::rest {

/**
 * @brief Adds parts to every endpoint of the apis and endpoints it wraps: `with<security<bearer_auth>, users::api>`.
 *
 * Its members are parts (query<>, header<>, security<>...) and apis or endpoints, in any order. The inherited parts
 * come after the endpoint's own, in its arguments too. Used inside an api<>.
 */
template <class... Members>
struct with {
  static_assert(
      ((detail::Part<Members> || detail::Api<Members> || detail::Endpoint<Members> || detail::With<Members>) && ...),
      "quoll::rest::with: a member is neither a part nor an api<>, a route or a group"
  );
  static_assert(
      ((!std::same_as<typename detail::part_category_of<Members>::type, detail::events_part>) && ...),
      "quoll::rest::with: events<> is a part of one endpoint, not one to share"
  );

  using contract_category = detail::with_category;
  using members = detail::type_list<Members...>;
};

namespace detail {

template <class Routes>
struct holds_operation;

template <class... Routes>
struct holds_operation<type_list<Routes...>> : std::bool_constant<(Operation<typename Routes::endpoint> || ...)> {};

template <class Member>
[[nodiscard]] consteval bool no_route_under_a_prefix();

template <class... Members>
[[nodiscard]] consteval bool no_route_under_a_prefix(type_list<Members...>) {
  return (no_route_under_a_prefix<Members>() && ...);
}

// Whether a member api<> with a prefix holds no route, in a group or not: two of its routes and another's, the same
// declaration, would be one type.
template <class Member>
[[nodiscard]] consteval bool no_route_under_a_prefix() {
  if constexpr (Api<Member>) {
    return Member::prefix.empty() || !holds_operation<typename Member::routes>::value;
  } else if constexpr (With<Member>) {
    return no_route_under_a_prefix(typename Member::members{});
  } else {
    return true;
  }
}

} // namespace detail

/**
 * @brief Routes and the options they share, with no prefix: `rest::group<rest::security<rest::bearer_auth>, list,
 * get>`. Its members are options (security<>, header<>...), routes and groups, in any order; what it holds comes after
 * a route's own arguments, in its arguments too. Groups nest; only api<> has a prefix, so that a route's string says
 * its whole path below it.
 */
template <class... Members>
struct group : with<Members...> {};

/**
 * @brief A group of endpoints under the path `Prefix`: endpoints, nested apis and with<>.
 *
 * Paths join: `api<"/api/v1", api<"/users", endpoint<..., "/{id:int64}">>>` serves `/api/v1/users/{id:int64}`, and
 * an endpoint path of `/` is the prefix itself. A prefix may have parameters, which come first in the arguments of
 * its endpoints. Checked at compile time: endpoint names are unique, and no two endpoints share a method and a path.
 */
template <fixed_string Prefix, class... Members>
struct api {
  static_assert(detail::checked_path<Prefix>::valid);
  static_assert(Prefix.view().empty() || Prefix.view().back() != '/', "quoll::rest::api: the prefix ends with '/'");
  static_assert(
      ((detail::Api<Members> || detail::Endpoint<Members> || detail::With<Members>) && ...),
      "quoll::rest::api: a member is not an endpoint<>, api<> or with<>"
  );

  static_assert(
      (detail::no_route_under_a_prefix<Members>() && ...),
      "quoll::rest::api: routes go in a rest::group, with no prefix: only the api has one, so that a route's string "
      "says its whole path"
  );

  using contract_category = detail::api_category;
  using routes = detail::concat_t<detail::type_list<>, typename detail::routes_of<Prefix, Members>::type...>;

  static_assert(detail::distinct_names(routes{}), "quoll::rest::api: two endpoints have the same name");
  static_assert(detail::distinct_operations(routes{}), "quoll::rest::api: two endpoints have the same method and path");

  /** @brief How this api serves `Endpoint`: its full path, arguments, responses and security schemes. */
  template <class Endpoint>
  using route = detail::find_route_t<Endpoint, routes>;

  static constexpr std::string_view prefix = Prefix.view();
};

} // namespace quoll::rest

#endif // QUOLL_REST_API_HPP
