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
#include <quoll/rest/body.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/query.hpp>
#include <quoll/rest/security.hpp>

#include <glaze/core/reflect.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

template <fixed_string Path, class Indices = std::make_index_sequence<checked_path<Path>::parsed.size>>
struct path_argument_types;

template <fixed_string Path, std::size_t... Index>
struct path_argument_types<Path, std::index_sequence<Index...>> {
  using type = type_list<typename param_kind_type<checked_path<Path>::parsed.params[Index].kind>::type...>;
};

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
  for (auto const name : glz::reflect<Struct>::keys) {
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

// An endpoint as an api serves it: its full path (the prefixes of the apis around it, then its own path) and the
// parts inherited from the with<> around it, which come after its own.
template <class Endpoint, fixed_string Path, class Inherited>
struct route {
  static_assert(checked_path<Path>::valid);

  using endpoint = Endpoint;
  using inherited = Inherited;
  using parts = concat_t<typename Endpoint::parts, Inherited>;

  // The endpoint checks its own parts; these hold with the parts inherited from with<> too.
  static_assert(count_bodies(parts{}) <= 1, "quoll::rest::with: the endpoint gets more than one body<>");
  static_assert(count_bodies(parts{}) == 0 ||
                    (Endpoint::method != http::verb::get && Endpoint::method != http::verb::head),
                "quoll::rest::with: a GET or HEAD endpoint gets a body<> (fetch rejects one)");
  static_assert(count_bodies(parts{}) == 0 || Endpoint::method != http::verb::trace,
                "quoll::rest::with: a TRACE endpoint gets a body<> (RFC 9110, 9.3.8)");
  static_assert(distinct_statuses(parts{}), "quoll::rest::with: the endpoint gets two returns<> with the same status");

  // The arguments of a call and of the service: path parameters, then argument parts in order.
  using arguments = as_tuple_t<concat_t<typename path_argument_types<Path>::type,
                                        typename part_argument_types<filter_t<is_argument_part, parts>>::type>>;
  using responses = filter_t<is_response_part, parts>;
  using schemes = typename security_schemes<filter_t<is_security_part, parts>>::type;

  static_assert(distinct_places(filter_t<is_argument_part, parts>{}, schemes{}),
                "quoll::rest::endpoint: it gets the same field or query parameter twice");

  static constexpr auto path_value = Path;
  static constexpr std::string_view path = path_value.view();
};

// A literal of a checked template, as the router compares it: decoded.
[[nodiscard]] consteval std::string decoded_literal(std::string_view literal) {
  std::string decoded;

  for (std::size_t index = 0; index < literal.size(); ++index) {
    if (literal[index] != '%') {
      decoded += literal[index];
      continue;
    }

    decoded += static_cast<char>(http::detail::hex_value(literal[index + 1]) * 16 +
                                 http::detail::hex_value(literal[index + 2]));
    index += 2;
  }

  return decoded;
}

// Whether two operations would be routed alike: the same method, and the router's same shape, the same decoded
// literals and the same kind of parameter in each place, whatever the names: `/a` and `/%61` are one, `{name}` and
// `{name:path}` two.
[[nodiscard]] consteval bool same_operation(http::verb first_method, std::string_view first, http::verb second_method,
                                            std::string_view second) {
  if (first_method != second_method) {
    return false;
  }

  auto const left = http::detail::parse_template(first);
  auto const right = http::detail::parse_template(second);

  // A template that is wrong has its own static_assert: it compares as different.
  if (left.error != http::detail::template_error::none || right.error != http::detail::template_error::none ||
      left.segments.size() != right.segments.size()) {
    return false;
  }

  for (std::size_t index = 0; index < left.segments.size(); ++index) {
    auto const &one = left.segments[index];
    auto const &other = right.segments[index];

    if (one.kind != other.kind) {
      return false;
    }

    if (one.kind == http::detail::segment_kind::literal && decoded_literal(one.text) != decoded_literal(other.text)) {
      return false;
    }
  }

  return true;
}

template <class... Routes>
[[nodiscard]] consteval bool distinct_names(type_list<Routes...>) {
  constexpr std::array<std::string_view, sizeof...(Routes)> names{Routes::endpoint::name...};

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
  constexpr std::array<http::verb, sizeof...(Routes)> methods{Routes::endpoint::method...};
  constexpr std::array<std::string_view, sizeof...(Routes)> paths{Routes::path...};

  for (std::size_t first = 0; first < paths.size(); ++first) {
    for (std::size_t second = first + 1; second < paths.size(); ++second) {
      if (same_operation(methods[first], paths[first], methods[second], paths[second])) {
        return false;
      }
    }
  }

  return true;
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_ROUTE_HPP
