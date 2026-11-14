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

#ifndef QUOLL_REST_DETAIL_ROUTE_HPP
#define QUOLL_REST_DETAIL_ROUTE_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/body.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/field_names.hpp>
#include <quoll/rest/detail/join_strings.hpp>
#include <quoll/rest/detail/operation_concept.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/query.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/query_params.hpp>
#include <quoll/rest/security.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

template <class Parts>
struct part_argument_types;

template <class... Parts>
struct part_argument_types<type_list<Parts...>> {
  using type = type_list<typename Parts::argument_type...>;
};

template <class Parts>
struct security_schemes;

template <class... Parts>
struct security_schemes<type_list<Parts...>> {
  using type = concat_t<type_list<>, typename Parts::schemes...>;
};

// Where a part or a security scheme puts something of a request, by name.
enum class named_place : unsigned char { nowhere, field, query };

struct place_of_part {
  named_place place = named_place::nowhere;
  std::string_view name;
};

template <class Part>
inline constexpr place_of_part place_of{};

template <fixed_string Name, class T>
inline constexpr place_of_part place_of<header<Name, T>>{named_place::field, Name.view()};

template <fixed_string Name, class T>
inline constexpr place_of_part place_of<query<Name, T>>{named_place::query, Name.view()};

template <class T, class BodyCodec>
inline constexpr place_of_part place_of<body<T, BodyCodec>>{named_place::field, "Content-Type"};

template <>
inline constexpr place_of_part place_of<bearer_auth>{named_place::field, "Authorization"};

template <fixed_string Header>
inline constexpr place_of_part place_of<api_key<Header>>{named_place::field, Header.view()};

template <fixed_string Param>
inline constexpr place_of_part place_of<api_key_query<Param>>{named_place::query, Param.view()};

template <class Part>
consteval void add_places(std::type_identity<Part>, std::vector<place_of_part> &places) {
  places.push_back(place_of<Part>);
}

template <class Struct>
consteval void add_places(std::type_identity<query_params<Struct>>, std::vector<place_of_part> &places) {
  for (auto const name : field_names<Struct>) {
    places.push_back({named_place::query, name});
  }
}

// Whether no two of them ask for the same place: what comes twice is refused, so that a call could never succeed.
template <class... Arguments, class... Schemes>
[[nodiscard]] consteval bool distinct_places(type_list<Arguments...>, type_list<Schemes...>) {
  std::vector<place_of_part> places;

  (add_places(std::type_identity<Arguments>{}, places), ...);
  (add_places(std::type_identity<Schemes>{}, places), ...);

  for (std::size_t first = 0; first < places.size(); ++first) {
    for (std::size_t second = first + 1; second < places.size(); ++second) {
      if (places[first].place == named_place::nowhere || places[first].place != places[second].place) {
        continue;
      }

      auto const same = places[first].place == named_place::field
                            ? http::detail::iequals(places[first].name, places[second].name)
                            : places[first].name == places[second].name;

      if (same) {
        return false;
      }
    }
  }

  return true;
}

[[nodiscard]] consteval std::size_t method_size(http::verb method) { return http::to_string(method).size(); }

template <http::verb Method>
[[nodiscard]] consteval auto method_string() {
  fixed_string<method_size(Method) + 1> name;

  std::ranges::copy(http::to_string(Method), name.value);
  return name;
}

// The name of an operation that gives none: its method and its whole path, `GET /api/notes/{id}`, unique in an api as
// the router refuses two routes of one method and path.
template <http::verb Method, fixed_string Path>
inline constexpr auto default_operation_name = join_strings<method_string<Method>(), " ", Path>();

// Its own name, or the one above, built only for an operation with none.
template <class Endpoint, fixed_string Path>
[[nodiscard]] consteval std::string_view operation_name_of() {
  if constexpr (Endpoint::name.empty()) {
    return default_operation_name<Endpoint::method, Path>.view();
  } else {
    return Endpoint::name;
  }
}

// The name of an argument part, as a call by name gives it: a header's; none for a body or a query_params<>.
template <class Part>
inline constexpr std::string_view argument_part_name{};

template <fixed_string Name, class T>
inline constexpr std::string_view argument_part_name<header<Name, T>> = Name.view();

// The names of a route's arguments, in their order: its `{parameters}`, then its argument parts past its query's, the
// headers by their names.
template <class Endpoint, class... ArgumentParts>
[[nodiscard]] consteval auto argument_names_of(type_list<ArgumentParts...> /*parts*/) {
  constexpr auto named = Endpoint::parameter_names;
  constexpr std::size_t path_count = Endpoint::path_types::size;
  std::array<std::string_view, path_count + sizeof...(ArgumentParts)> names{};
  std::size_t at = 0;

  for (; at < named.size() && at < names.size(); ++at) {
    names[at] = named[at];
  }

  at = path_count;
  ((names[at] = at < named.size() ? names[at] : argument_part_name<ArgumentParts>, ++at), ...);
  return names;
}

template <class Part>
inline constexpr bool is_header_part = false;

template <fixed_string Name, class T>
inline constexpr bool is_header_part<header<Name, T>> = true;

// Whether each argument of a route is a header, whose name a call by name gives without case.
template <class Endpoint, class... ArgumentParts>
[[nodiscard]] consteval auto argument_headers_of(type_list<ArgumentParts...> /*parts*/) {
  constexpr std::size_t path_count = Endpoint::path_types::size;
  std::array<bool, path_count + sizeof...(ArgumentParts)> headers{};
  std::size_t at = path_count;

  ((headers[at++] = is_header_part<ArgumentParts>), ...);
  return headers;
}

// A route as an api serves it: its full path (the api's prefix, then its own path) and what the groups around it give
// all their routes, security<> and header<>, which come after its own arguments.
template <class Endpoint, fixed_string Path, class Inherited>
struct route {
  static_assert(checked_path<Path>::valid);

  using endpoint = Endpoint;
  using inherited = Inherited;
  using parts = concat_t<typename Endpoint::parts, Inherited>;
  // Its own: the api's prefix has none.
  using path_types = typename Endpoint::path_types;

  // The arguments of a call and of its handler: path parameters, then argument parts in order.
  using arguments =
      as_tuple_t<concat_t<path_types, typename part_argument_types<filter_t<is_argument_part, parts>>::type>>;
  using responses = filter_t<is_response_part, parts>;
  using schemes = typename security_schemes<filter_t<is_security_part, parts>>::type;

  // The name of each argument, as a call by name gives it: empty for one it cannot name.
  static constexpr auto argument_names = argument_names_of<Endpoint>(filter_t<is_argument_part, parts>{});
  static constexpr auto argument_is_header = argument_headers_of<Endpoint>(filter_t<is_argument_part, parts>{});

  static_assert(
      distinct_places(filter_t<is_argument_part, parts>{}, schemes{}),
      "quoll::rest: a route gets the same header or query parameter twice, its groups' and its security's included"
  );

  static constexpr auto path_value = Path;
  static constexpr std::string_view path = path_value.view();
  // The name and the method of the operation, which the router, the client and the checks of an api read.
  static constexpr std::string_view name = operation_name_of<Endpoint, Path>();
  static constexpr http::verb method = Endpoint::method;
};

// The shape of a template as the router compares it: the kind of each segment, and each literal decoded. Nothing for a
// template that is wrong, which has its own static_assert. A NUL ends each segment, as no checked literal holds one, so
// that `/a%2Fb`, one segment, and `/a/b`, two, differ.
[[nodiscard]] consteval std::optional<std::string> route_shape(std::string_view path) {
  auto const parsed = http::detail::parse_template(path);

  if (parsed.error != http::detail::template_error::none) {
    return std::nullopt;
  }

  std::string shape;

  for (auto const &segment : parsed.segments) {
    switch (segment.kind) {
    case http::detail::segment_kind::literal:
      shape += 'l' + http::detail::decoded_literal(segment.text);
      break;
    case http::detail::segment_kind::param:
      shape += 'p';
      break;
    case http::detail::segment_kind::rest:
      shape += 'r';
      break;
    }

    shape += '\0';
  }

  return shape;
}

// Whether two operations would be routed alike, from their shapes: the same method, and the same decoded literals and
// kind of parameter in each place, whatever the names: `/a` and `/%61` are one, `{name}` and `{name:path}` two.
[[nodiscard]] consteval bool same_shape(
    http::verb first_method, std::optional<std::string> const &first, http::verb second_method,
    std::optional<std::string> const &second
) {
  return first_method == second_method && first && second && *first == *second;
}

[[nodiscard]] consteval bool
same_operation(http::verb first_method, std::string_view first, http::verb second_method, std::string_view second) {
  return same_shape(first_method, route_shape(first), second_method, route_shape(second));
}

template <class... Routes>
[[nodiscard]] consteval bool distinct_names(type_list<Routes...>) {
  constexpr std::array<std::string_view, sizeof...(Routes)> names{Routes::name...};

  for (std::size_t first = 0; first < names.size(); ++first) {
    for (std::size_t second = first + 1; second < names.size(); ++second) {
      if (names[first] == names[second]) {
        return false;
      }
    }
  }

  return true;
}

template <class... Routes>
[[nodiscard]] consteval bool distinct_operations(type_list<Routes...>) {
  constexpr std::array<http::verb, sizeof...(Routes)> methods{Routes::method...};
  constexpr std::array<std::string_view, sizeof...(Routes)> paths{Routes::path...};
  // Once each, before the pairs. A loop, as GCC 13 refuses a consteval call in a pack expansion here.
  std::array<std::optional<std::string>, sizeof...(Routes)> shapes{};

  for (std::size_t index = 0; index < paths.size(); ++index) {
    shapes[index] = route_shape(paths[index]);
  }

  for (std::size_t first = 0; first < shapes.size(); ++first) {
    for (std::size_t second = first + 1; second < shapes.size(); ++second) {
      if (same_shape(methods[first], shapes[first], methods[second], shapes[second])) {
        return false;
      }
    }
  }

  return true;
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_ROUTE_HPP
