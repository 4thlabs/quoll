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

#ifndef QUOLL_REST_DETAIL_RESPONSE_VALUE_HPP
#define QUOLL_REST_DETAIL_RESPONSE_VALUE_HPP

#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace quoll::rest::detail {

// The value of one declared response; a distinct type per returns<>, even when two have the same body type.
template <class Response, class T = typename Response::type>
struct response_value {
  using response = Response;

  T value;
};

template <class Response>
struct response_value<Response, void> {
  using response = Response;
};

template <class Responses>
struct response_variant;

template <class... Responses>
struct response_variant<type_list<Responses...>> {
  using type = std::variant<response_value<Responses>...>;
};

// Which responses have body type T, or status Status and body type T.
template <class T, class... Responses>
[[nodiscard]] consteval std::array<bool, sizeof...(Responses)> with_type(type_list<Responses...>) {
  return {std::same_as<typename Responses::type, T>...};
}

template <auto Status, class T, class... Responses>
[[nodiscard]] consteval std::array<bool, sizeof...(Responses)> with_status(type_list<Responses...>) {
  return {(Responses::status == Status && std::same_as<typename Responses::type, T>)...};
}

template <auto Status, class... Responses>
[[nodiscard]] consteval std::array<bool, sizeof...(Responses)> with_status_only(type_list<Responses...>) {
  return {(Responses::status == Status)...};
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_RESPONSE_VALUE_HPP
