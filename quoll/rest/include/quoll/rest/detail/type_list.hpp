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

#ifndef QUOLL_REST_DETAIL_TYPE_LIST_HPP
#define QUOLL_REST_DETAIL_TYPE_LIST_HPP

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace quoll::rest::detail {

template <class... Ts>
struct type_list {
  static constexpr std::size_t size = sizeof...(Ts);
};

template <class... Lists>
struct concat;

template <>
struct concat<> {
  using type = type_list<>;
};

template <class... Ts>
struct concat<type_list<Ts...>> {
  using type = type_list<Ts...>;
};

template <class... Ts, class... Us, class... Rest>
struct concat<type_list<Ts...>, type_list<Us...>, Rest...> {
  using type = typename concat<type_list<Ts..., Us...>, Rest...>::type;
};

template <class... Lists>
using concat_t = typename concat<Lists...>::type;

// The elements of List for which Predicate<T>::value holds, in order.
template <template <class> class Predicate, class List>
struct filter;

template <template <class> class Predicate, class... Ts>
struct filter<Predicate, type_list<Ts...>> {
  using type = concat_t<type_list<>, std::conditional_t<Predicate<Ts>::value, type_list<Ts>, type_list<>>...>;
};

template <template <class> class Predicate, class List>
using filter_t = typename filter<Predicate, List>::type;

template <class List>
struct as_tuple;

template <class... Ts>
struct as_tuple<type_list<Ts...>> {
  using type = std::tuple<Ts...>;
};

template <class List>
using as_tuple_t = typename as_tuple<List>::type;

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_TYPE_LIST_HPP
