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

#ifndef QUOLL_REST_DETAIL_EXPRESSION_HPP
#define QUOLL_REST_DETAIL_EXPRESSION_HPP

#include <quoll/http/detail/route_template.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace quoll::rest::detail {

// `Length` characters of `S` from `Begin`, as a fixed_string.
template <fixed_string S, std::size_t Begin, std::size_t Length>
[[nodiscard]] consteval auto slice_string() {
  fixed_string<Length + 1> sliced;

  std::ranges::copy(S.view().substr(Begin, Length), sliced.value);
  return sliced;
}

// What a route's expression says, `"/notes/{id}?fields={fields}"`: its path, before '?', and its query, read at
// compile time by the router's grammar (http::detail::parse_template).
template <fixed_string Expr>
struct expression {
  [[nodiscard]] static consteval std::size_t find_question() {
    auto const at = http::detail::find_char(Expr.view(), '?');

    return at == std::string_view::npos ? Expr.view().size() : at;
  }

  static constexpr std::size_t question = find_question();
  static constexpr auto path = slice_string<Expr, 0, question>();

  [[nodiscard]] static consteval http::detail::template_error error() {
    return http::detail::parse_template(Expr.view()).error;
  }

  [[nodiscard]] static consteval std::size_t path_count() {
    auto const parsed = http::detail::parse_template(Expr.view());

    return static_cast<std::size_t>(std::ranges::count_if(parsed.segments, [](auto const &segment) {
      return segment.kind != http::detail::segment_kind::literal;
    }));
  }

  [[nodiscard]] static consteval std::size_t query_count() {
    return http::detail::parse_template(Expr.view()).query.size();
  }

  // Whether a parameter of the path says its type, `{id:int64}`, which a route's C++ types say instead; `{x:path}`,
  // the rest of the path, is the one the string says.
  [[nodiscard]] static consteval bool typed_in_path() {
    auto const parsed = http::detail::parse_template(Expr.view());

    return std::ranges::any_of(parsed.segments, [](auto const &segment) {
      return segment.kind == http::detail::segment_kind::param && !segment.type.empty();
    });
  }

  // Whether the path ends with `{name:path}`, the rest of it.
  [[nodiscard]] static consteval bool rest_of_path() {
    auto const parsed = http::detail::parse_template(Expr.view());

    return !parsed.segments.empty() && parsed.segments.back().kind == http::detail::segment_kind::rest;
  }

  // Whether the query has a fixed `key=value`.
  [[nodiscard]] static consteval bool has_fixed_value() {
    auto const parsed = http::detail::parse_template(Expr.view());

    return std::ranges::any_of(parsed.query, [](auto const &entry) { return entry.param.empty(); });
  }

  // Whether every key of the query is written plain, with no escape nor '+': as the name of a query<> it is sent.
  [[nodiscard]] static consteval bool plain_keys() {
    auto const parsed = http::detail::parse_template(Expr.view());

    return std::ranges::all_of(parsed.query, [](auto const &entry) {
      return entry.key.find_first_of("%+") == std::string_view::npos;
    });
  }

  // Where the key of the `Index`th pair of the query is, in the expression.
  [[nodiscard]] static consteval std::size_t key_begin(std::size_t index) {
    auto const parsed = http::detail::parse_template(Expr.view());

    return static_cast<std::size_t>(parsed.query[index].key.data() - Expr.view().data());
  }

  [[nodiscard]] static consteval std::size_t key_size(std::size_t index) {
    return http::detail::parse_template(Expr.view()).query[index].key.size();
  }
};

// The key of the `Index`th pair of the query of `Expr`, as a fixed_string: the name of its query<> part.
template <fixed_string Expr, std::size_t Index>
inline constexpr auto query_key =
    slice_string<Expr, expression<Expr>::key_begin(Index), expression<Expr>::key_size(Index)>();

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_EXPRESSION_HPP
