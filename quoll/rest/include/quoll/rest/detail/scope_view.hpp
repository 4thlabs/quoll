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

#ifndef QUOLL_REST_DETAIL_SCOPE_VIEW_HPP
#define QUOLL_REST_DETAIL_SCOPE_VIEW_HPP

#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/routes.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

namespace quoll::rest::detail {

// The arguments a scope fixes, and those left to a call: the first `Count` of a tuple, and the others.
template <std::size_t Count, class Tuple, class Indices = std::make_index_sequence<Count>>
struct take_front;

template <std::size_t Count, class... Types, std::size_t... Index>
struct take_front<Count, std::tuple<Types...>, std::index_sequence<Index...>> {
  using type = std::tuple<std::tuple_element_t<Index, std::tuple<Types...>>...>;
};

template <std::size_t Count, class Tuple, class Indices = std::make_index_sequence<std::tuple_size_v<Tuple> - Count>>
struct drop_front;

template <std::size_t Count, class... Types, std::size_t... Index>
struct drop_front<Count, std::tuple<Types...>, std::index_sequence<Index...>> {
  using type = std::tuple<std::tuple_element_t<Count + Index, std::tuple<Types...>>...>;
};

// The parameters of a prefix: what its braces open, as a checked template has no other.
[[nodiscard]] consteval std::size_t parameters_in(std::string_view prefix) {
  return static_cast<std::size_t>(std::ranges::count(prefix, '{'));
}

// How many path parameters a scope of `Scope` fixes for `Endpoint`, as `Api` serves it: those of the prefixes above
// `Scope` and of its own, not those of the apis nested in it. They come first in the endpoint's arguments.
template <class Api, class Scope, class Endpoint>
inline constexpr std::size_t prefix_arity =
    checked_path<Api::template route<Endpoint>::path_value>::parsed.size + parameters_in(Scope::prefix) -
    checked_path<Scope::template route<Endpoint>::path_value>::parsed.size;

// Whether every endpoint of `Inner` is one of `Outer`, once: an api nested in it, rather than a sibling whose prefixes
// differ, whose arguments a scope would then fix wrongly.
template <class Outer, class... Routes>
[[nodiscard]] consteval bool nested_in(type_list<Routes...>) {
  return ((matching_routes<typename Routes::endpoint, typename Outer::routes>::type::size == 1) && ...);
}

// What a scope fixes, computed only for an api nested in the client's: for another, what it would compute fails with
// messages of its own, beside the static_assert that says why.
template <bool Nested, class Api, class Scope>
struct scope_prefix {
  static constexpr std::size_t arity = 0;
  using type = std::tuple<>;
};

template <class Api, class Scope>
struct scope_prefix<true, Api, Scope> {
  using first_endpoint = typename front<typename Scope::routes>::type::endpoint;

  static constexpr std::size_t arity = prefix_arity<Api, Scope, first_endpoint>;
  using type = typename take_front<arity, typename Api::template route<first_endpoint>::arguments>::type;
};

// `Api` as a scope of it sees it: the endpoints of `Scope` alone, their prefixes' parameters fixed. client_calls reads
// the routes and their arguments from it, as from an api.
template <class Api, class Scope>
struct scope_view {
  static_assert(Scope::routes::size != 0, "quoll::rest::client::scope: the api has no endpoint");

  static constexpr bool nested = Scope::routes::size != 0 && nested_in<Api>(typename Scope::routes{});

  static_assert(
      Scope::routes::size == 0 || nested, "quoll::rest::client::scope: the api is not nested in the client's"
  );

  static constexpr std::size_t arity = scope_prefix<nested, Api, Scope>::arity;

  template <class Endpoint>
  struct scoped_route {
    using arguments = typename drop_front<arity, typename Api::template route<Endpoint>::arguments>::type;
  };

  // Only the endpoints of `Scope`: another is refused with find_route_t's message.
  template <class Endpoint>
  using route = scoped_route<typename find_route_t<Endpoint, typename Scope::routes>::endpoint>;

  template <class Routes>
  struct scoped_routes;

  template <class... Routes>
  struct scoped_routes<type_list<Routes...>> {
    using type = type_list<scoped_route<typename Routes::endpoint>...>;
  };

  using routes = typename scoped_routes<typename Scope::routes>::type;

  // The values a scope holds, of the types its endpoints take them as.
  using prefix = typename scope_prefix<nested, Api, Scope>::type;
};

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_SCOPE_VIEW_HPP
