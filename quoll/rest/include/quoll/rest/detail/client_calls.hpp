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

#ifndef QUOLL_REST_DETAIL_CLIENT_CALLS_HPP
#define QUOLL_REST_DETAIL_CLIENT_CALLS_HPP

#include <quoll/rest/detail/type_list.hpp>

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

namespace quoll::rest::detail {

// The type of argument `Index` of a call, or no `type` past the last one: an overload of another arity then drops out
// of overload resolution instead of failing to compile.
template <class Arguments, std::size_t Index>
struct nth_argument {};

template <class... Arguments, std::size_t Index>
  requires(Index < sizeof...(Arguments))
struct nth_argument<std::tuple<Arguments...>, Index> {
  using type = std::tuple_element_t<Index, std::tuple<Arguments...>>;
};

// call / try_call taking exactly `sizeof...(Index)` arguments of the types the endpoint computes: not deduced, so that
// a braced list (`{.limit = 20}`) converts to its parameter.
template <class Api, class Endpoint>
using arguments_of = typename Api::template route<Endpoint>::arguments;

// The client is still incomplete when these bases are instantiated: they compute the arguments from the api.
template <class Client, class Api, class Indices>
struct client_calls;

template <class Client, class Api, std::size_t... Index>
struct client_calls<Client, Api, std::index_sequence<Index...>> {
  template <class Endpoint>
    requires(std::tuple_size_v<arguments_of<Api, Endpoint>> == sizeof...(Index))
  [[nodiscard]] auto call(typename nth_argument<arguments_of<Api, Endpoint>, Index>::type... args) const {
    return static_cast<Client const &>(*this).template call_with<Endpoint>(
        arguments_of<Api, Endpoint>{std::move(args)...});
  }

  template <class Endpoint>
    requires(std::tuple_size_v<arguments_of<Api, Endpoint>> == sizeof...(Index))
  [[nodiscard]] auto try_call(typename nth_argument<arguments_of<Api, Endpoint>, Index>::type... args) const {
    return static_cast<Client const &>(*this).template try_call_with<Endpoint>(
        arguments_of<Api, Endpoint>{std::move(args)...});
  }
};

// The most arguments a call to an endpoint of the api takes: the client has an overload per arity up to it.
template <class... Routes>
[[nodiscard]] consteval std::size_t max_arity(type_list<Routes...>) {
  std::size_t largest = 0;

  ((largest = std::max(largest, std::tuple_size_v<typename Routes::arguments>)), ...);
  return largest;
}

// Every arity up to the last of `Arities`.
template <class Client, class Api, class Arities>
struct client_call_set;

template <class Client, class Api, std::size_t... Arity>
struct client_call_set<Client, Api, std::index_sequence<Arity...>>
    : client_calls<Client, Api, std::make_index_sequence<Arity>>... {
  using client_calls<Client, Api, std::make_index_sequence<Arity>>::call...;
  using client_calls<Client, Api, std::make_index_sequence<Arity>>::try_call...;
};

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_CALLS_HPP
