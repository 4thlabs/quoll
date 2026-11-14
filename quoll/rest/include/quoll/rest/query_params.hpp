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

#ifndef QUOLL_REST_QUERY_PARAMS_HPP
#define QUOLL_REST_QUERY_PARAMS_HPP

#include <quoll/rest/detail/aggregate_fields.hpp>
#include <quoll/rest/detail/lists_fields.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <concepts>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace quoll::rest {

/**
 * @brief A part of a route: one query parameter per field of the aggregate `Struct`, named as the field is declared,
 * or as its `rest_names` give (see names), each field a Parameter, an std::optional or an std::vector of one. For
 * operations with many optional parameters: the caller fills a struct with designated initializers. A struct of 64
 * fields at most, with no base class, each field with a default value, none a reference nor a pointer to characters; or
 * a struct listing its fields, `rest_fields`, as one with a base class (see names).
 */
template <class Struct>
struct query_params {
  static_assert(
      !std::ranges::range<Struct>, "quoll::rest::query_params: the type is a struct of fields, not an array or a range"
  );
  static_assert(
      detail::ListsFields<Struct> || std::is_aggregate_v<Struct>,
      "quoll::rest::query_params: the type is not an aggregate, nor a struct listing its fields (rest_fields)"
  );
  static_assert(
      detail::ListsFields<Struct> || detail::within_fields<Struct>(),
      "quoll::rest::query_params: the struct has more than 64 fields"
  );

  static_assert(
      !detail::ListsFields<Struct> || std::default_initializable<Struct>,
      "quoll::rest::query_params: a struct listing its fields is made with none of them given"
  );

  using part_category = detail::argument_part;
  using argument_type = Struct;
};

} // namespace quoll::rest

#endif // QUOLL_REST_QUERY_PARAMS_HPP
