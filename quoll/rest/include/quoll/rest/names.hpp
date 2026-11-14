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

#ifndef QUOLL_REST_NAMES_HPP
#define QUOLL_REST_NAMES_HPP

#include <quoll/rest/fixed_string.hpp>

#include <string_view>
#include <type_traits>

namespace quoll::rest {

/**
 * @brief The name a field of a query_params<> struct or a form goes by, when it is not the one it is declared with:
 * `name<"api-version", &search::api_version>`. See names.
 */
template <fixed_string Name, auto Member>
struct name {
  static_assert(!Name.view().empty(), "quoll::rest::name: the name is empty");
  static_assert(std::is_member_object_pointer_v<decltype(Member)>, "quoll::rest::name: names a field, by its pointer");

  static constexpr std::string_view value = Name.view();
  static constexpr auto member = Member;
};

/**
 * @brief The fields of a struct that go by another name than the one they are declared with, in a query_params<> or a
 * form; the others keep theirs.
 *
 * @code
 * struct charge {
 *   std::string order_id;
 *   std::int64_t amount = 0;
 *   std::optional<std::string> api_version;
 *
 *   // Only the names that are not C++ names: amount keeps its own.
 *   using rest_names = rest::names<
 *       rest::name<"metadata[order_id]", &charge::order_id>,
 *       rest::name<"api-version", &charge::api_version>>;
 * };
 * @endcode
 *
 * A struct the reflection does not read, as one with a base class, lists all its fields as `rest_fields`, of the same
 * form, read as listed. A name of a field of another struct does not compile, nor do two fields of one name, nor a
 * struct with both. JSON names are given by the
 * JSON codec's own description, as Glaze's `glz::meta`.
 */
template <class... Names>
struct names;

template <fixed_string... Name, auto... Member>
struct names<name<Name, Member>...> {};

} // namespace quoll::rest

#endif // QUOLL_REST_NAMES_HPP
