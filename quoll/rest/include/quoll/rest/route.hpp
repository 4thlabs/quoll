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

#ifndef QUOLL_REST_ROUTE_HPP
#define QUOLL_REST_ROUTE_HPP

#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/operation.hpp>
#include <quoll/rest/fixed_string.hpp>

namespace quoll::rest {

/**
 * @brief A route answering GET: its expression, its path and query below the api's prefix, its result, then a type
 * for each of its `{parameters}`, in order, then its headers.
 *
 * @code
 * using get = rest::get<"/notes/{id}?fields={fields}", note, std::int64_t, std::optional<std::string>>;
 * @endcode
 *
 * Its result is answered 200, or 204 for `void`; `created<T>` and `accepted<T>` say 201 and 202; `text`, `bytes`,
 * `json<T>`, `lenient_json<T>`, `form<T>` and `encoded<T, Codec>` say its format, JSON by default; `events<T>` makes
 * it a stream of server-sent events. A query parameter is a std::optional to be missing and a std::vector to repeat;
 * `query_params<S>` takes them as the fields of a struct. `header<"X-Id", T>` adds a header, `security<...>` a scheme,
 * `operation_id<"...">` a name. Each route is its own type, which its handler takes first (see service).
 */
template <fixed_string Expr, class Result, class... Args>
struct get : detail::operation<http::verb::get, Expr, Result, Args...> {};

/** @brief A route answering POST, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct post : detail::operation<http::verb::post, Expr, Result, Args...> {};

/** @brief A route answering PUT, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct put : detail::operation<http::verb::put, Expr, Result, Args...> {};

/** @brief A route answering PATCH, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct patch : detail::operation<http::verb::patch, Expr, Result, Args...> {};

/** @brief A route answering DELETE (`delete` is a keyword): see get. */
template <fixed_string Expr, class Result, class... Args>
struct del : detail::operation<http::verb::delete_, Expr, Result, Args...> {};

/** @brief A route answering HEAD: see get. */
template <fixed_string Expr, class Result, class... Args>
struct head : detail::operation<http::verb::head, Expr, Result, Args...> {};

/** @brief A route answering OPTIONS: see get. */
template <fixed_string Expr, class Result, class... Args>
struct options : detail::operation<http::verb::options, Expr, Result, Args...> {};

} // namespace quoll::rest

#endif // QUOLL_REST_ROUTE_HPP
