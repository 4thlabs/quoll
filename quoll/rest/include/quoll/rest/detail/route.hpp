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

#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

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
  static_assert(count_bodies(parts{}) == 0 || Endpoint::method != http::verb::connect,
                "quoll::rest::with: a CONNECT endpoint gets a body<> (what follows its head is the tunnel)");
  static_assert(distinct_statuses(parts{}), "quoll::rest::with: the endpoint gets two returns<> with the same status");

  // The arguments of a call and of the service: path parameters, then argument parts in order.
  using arguments = as_tuple_t<concat_t<typename path_argument_types<Path>::type,
                                        typename part_argument_types<filter_t<is_argument_part, parts>>::type>>;
  using responses = filter_t<is_response_part, parts>;
  using schemes = typename security_schemes<filter_t<is_security_part, parts>>::type;

  static constexpr auto path_value = Path;
  static constexpr std::string_view path = path_value.view();
};

// Whether two operations would be routed alike: same method, and same path once the parameter names are ignored.
[[nodiscard]] consteval bool same_operation(http::verb first_method, std::string_view first, http::verb second_method,
                                            std::string_view second) {
  if (first_method != second_method) {
    return false;
  }

  std::size_t left = 0;
  std::size_t right = 0;

  while (left < first.size() && right < second.size()) {
    if (first[left] == '{' && second[right] == '{') {
      auto const first_close = http::detail::find_char(first, '}', left);
      auto const second_close = http::detail::find_char(second, '}', right);

      // Checked templates have their braces; an unbalanced one compares as different rather than wrapping around.
      if (first_close == std::string_view::npos || second_close == std::string_view::npos) {
        return false;
      }

      left = first_close + 1;
      right = second_close + 1;
      continue;
    }

    if (first[left] != second[right]) {
      return false;
    }

    ++left;
    ++right;
  }

  return left == first.size() && right == second.size();
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
