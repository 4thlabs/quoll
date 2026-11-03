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

#ifndef QUOLL_REST_DETAIL_FIELDS_OF_HPP
#define QUOLL_REST_DETAIL_FIELDS_OF_HPP

#include <glaze/core/reflect.hpp>

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

// Calls `visit(name, field)` for each field of `value`, with the name Glaze gives it (its own, or its glz::meta
// name): the fields of a query_params<> struct, to fill (the server) or to read (a client, with a const value).
template <class T, class Visitor>
void for_each_field(T &value, Visitor &&visit) {
  using type = std::remove_const_t<T>;

  constexpr auto size = glz::reflect<type>::size;

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    if constexpr (glz::glaze_object_t<type>) {
      (visit(glz::reflect<type>::keys[Index], glz::get_member(value, glz::get<Index>(glz::reflect<type>::values))),
       ...);
    } else {
      (visit(glz::reflect<type>::keys[Index], glz::get_member(value, glz::get<Index>(glz::to_tie(value)))), ...);
    }
  }(std::make_index_sequence<size>{});
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_FIELDS_OF_HPP
