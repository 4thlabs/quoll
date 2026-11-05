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

#ifndef QUOLL_REST_ENDPOINT_HPP
#define QUOLL_REST_ENDPOINT_HPP

#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/category.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <string_view>

namespace quoll::rest {

/**
 * @brief One operation of an API: its name, method and path template, then its parts.
 *
 * - `Name` is mandatory, unique in its api (checked by api<>), the OpenAPI `operationId` and the name of the route.
 * - `Path` is a template with typed parameters, each a whole segment: `/users/{id:int64}`; the types are `int`,
 *   `int64`, `uint`, `uint64`, `double`, `bool`, `string` (the default, `{id}`) and `path` (the rest of the path,
 *   last). It is checked at compile time.
 * - `Parts`: query<>, query_params<>, header<>, at most one body<> (none on GET or HEAD), returns<> (one per status),
 *   security<>.
 *
 * The arguments of the endpoint, the same for a client's `call` and for the service, are its path parameters
 * (prefixes first, see api<>), then the argument parts in order, then the parts inherited from with<>.
 */
template <fixed_string Name, http::verb Method, fixed_string Path, class... Parts>
struct endpoint {
  static_assert(!Name.view().empty(), "quoll::rest::endpoint: the name is empty");
  static_assert(detail::checked_path<Path>::valid);
  static_assert((detail::Part<Parts> && ...), "quoll::rest::endpoint: a part is not query<>, query_params<>, "
                                              "header<>, body<>, returns<> or security<>");
  static_assert(detail::count_bodies(detail::type_list<Parts...>{}) <= 1,
                "quoll::rest::endpoint: more than one body<>");
  static_assert(detail::count_bodies(detail::type_list<Parts...>{}) == 0 ||
                    (Method != http::verb::get && Method != http::verb::head),
                "quoll::rest::endpoint: a GET or HEAD endpoint has no body<> (fetch rejects one)");
  static_assert(detail::count_bodies(detail::type_list<Parts...>{}) == 0 || Method != http::verb::connect,
                "quoll::rest::endpoint: a CONNECT endpoint has no body<> (what follows its head is the tunnel)");
  static_assert(detail::distinct_statuses(detail::type_list<Parts...>{}),
                "quoll::rest::endpoint: two returns<> have the same status");

  using contract_category = detail::endpoint_category;
  using parts = detail::type_list<Parts...>;

  static constexpr std::string_view name = Name.view();
  static constexpr http::verb method = Method;
  static constexpr std::string_view path = Path.view();
};

} // namespace quoll::rest

#endif // QUOLL_REST_ENDPOINT_HPP
