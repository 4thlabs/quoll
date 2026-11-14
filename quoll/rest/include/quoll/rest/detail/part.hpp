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

#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/parameter.hpp>

#include <concepts>
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

template <class P>
struct is_events_part : std::bool_constant<std::same_as<typename P::part_category, events_part>> {};

// The events<> part of a route, if it has one.
template <class Endpoint>
using events_of = filter_t<is_events_part, typename Endpoint::parts>;

template <class Endpoint>
inline constexpr bool has_events = events_of<Endpoint>::size > 0;

// The events<> part of a route that has one.
template <class Endpoint>
using events_part_of = typename front<events_of<Endpoint>>::type;

// The returns<> of a route's own result: one, or none for a void result or events<>; a group gives none.
template <class Endpoint>
using own_responses = filter_t<is_response_part, typename Endpoint::parts>;

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

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PART_HPP
