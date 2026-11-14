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

#ifndef QUOLL_HTTP_DETAIL_QUERY_PARAMETER_HPP
#define QUOLL_HTTP_DETAIL_QUERY_PARAMETER_HPP

#include <quoll/http/detail/path_parameter.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quoll::http::detail {

// A parameter of a route's query, `?q={q}`: a path parameter's type, required and given once; a std::optional of one,
// which may be missing; a std::vector of one, its values in order.
template <class T>
inline constexpr bool is_query_list = false;

template <class T>
inline constexpr bool is_query_list<std::vector<T>> = PathParameter<T>;

template <class T>
inline constexpr bool is_query_optional = false;

template <class T>
inline constexpr bool is_query_optional<std::optional<T>> = PathParameter<T>;

template <class T>
concept QueryParameter = PathParameter<T> || is_query_list<T> || is_query_optional<T>;

static_assert(QueryParameter<std::optional<int>> && QueryParameter<std::vector<std::string>>);
static_assert(!QueryParameter<std::optional<std::vector<int>>>);

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_QUERY_PARAMETER_HPP
