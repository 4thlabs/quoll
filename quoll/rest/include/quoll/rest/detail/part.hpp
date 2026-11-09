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

#ifndef QUOLL_REST_DETAIL_PART_HPP
#define QUOLL_REST_DETAIL_PART_HPP

#include <quoll/http/status.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/parameter.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <vector>

namespace quoll::rest::detail {

// What a part of an endpoint is: something the caller passes (query, header, body), a declared response, a stream of
// events, or the security schemes it requires.
struct argument_part {};

struct response_part {};

struct security_part {};

// A response that is a stream of events (events<>).
struct events_part {};

template <class P>
concept Part = requires { typename P::part_category; };

// The category of a member of with<>, none for one that is no part.
template <class Member>
struct part_category_of {
  using type = void;
};

template <Part Member>
struct part_category_of<Member> {
  using type = typename Member::part_category;
};

template <class P>
struct is_argument_part : std::bool_constant<std::same_as<typename P::part_category, argument_part>> {};

template <class P>
struct is_response_part : std::bool_constant<std::same_as<typename P::part_category, response_part>> {};

// Whether a part declares a response with a body.
template <class P>
[[nodiscard]] consteval bool declares_a_body_response() {
  if constexpr (std::same_as<typename P::part_category, response_part>) {
    return !std::is_void_v<typename P::type>;
  } else {
    return false;
  }
}

template <class... Parts>
[[nodiscard]] consteval bool any_body_response(type_list<Parts...>) {
  return (declares_a_body_response<Parts>() || ...);
}

template <class P>
struct is_events_part : std::bool_constant<std::same_as<typename P::part_category, events_part>> {};

template <class... Parts>
[[nodiscard]] consteval std::size_t count_events(type_list<Parts...>) {
  return (std::size_t{0} + ... + (is_events_part<Parts>::value ? 1 : 0));
}

// The events<> part of an endpoint, if it has one.
template <class Endpoint>
using events_of = filter_t<is_events_part, typename Endpoint::parts>;

template <class Endpoint>
inline constexpr bool has_events = events_of<Endpoint>::size > 0;

// The events<> part of an endpoint that has one.
template <class Endpoint>
using events_part_of = typename front<events_of<Endpoint>>::type;

// Whether one of `Parts` declares what is no error (below 400): a success, or a redirect: a returns<> with a 2xx
// status.
template <class... Parts>
[[nodiscard]] consteval bool any_success_response(type_list<Parts...>) {
  return ([]<class P>() {
    if constexpr (std::same_as<typename P::part_category, response_part>) {
      return static_cast<int>(P::status) < 400;
    } else {
      return false;
    }
  }.template operator()<Parts>() ||
          ...);
}

template <class P>
struct is_security_part : std::bool_constant<std::same_as<typename P::part_category, security_part>> {};

// The value of a query parameter or a header: a Parameter (required), an std::optional of one (may be omitted) or an
// std::vector of one (repeated).
template <class T>
struct is_parameter_value : std::bool_constant<Parameter<T>> {};

template <class T>
struct is_parameter_value<std::optional<T>> : std::bool_constant<Parameter<T>> {};

template <class T>
struct is_parameter_value<std::vector<T>> : std::bool_constant<Parameter<T>> {};

// Tags the security schemes, so that security<> accepts nothing else.
struct security_scheme {};

// The checks endpoint<> makes on its own parts, and route on them with the inherited ones.

template <class... Parts>
[[nodiscard]] consteval std::size_t count_bodies(type_list<Parts...>) {
  return (std::size_t{0} + ... + (is_argument_part<Parts>::value && requires { typename Parts::codec; } ? 1 : 0));
}

template <class... Parts>
[[nodiscard]] consteval bool distinct_statuses(type_list<Parts...>) {
  constexpr auto statuses = []<class... Responses>(type_list<Responses...>) {
    return std::array<http::status, sizeof...(Responses)>{Responses::status...};
  }(filter_t<is_response_part, type_list<Parts...>>{});

  for (std::size_t first = 0; first < statuses.size(); ++first) {
    for (std::size_t second = first + 1; second < statuses.size(); ++second) {
      if (statuses[first] == statuses[second]) {
        return false;
      }
    }
  }

  return true;
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PART_HPP
