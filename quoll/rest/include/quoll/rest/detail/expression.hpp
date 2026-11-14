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
#include <array>
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

// The most query parameters a route's expression has, and path parameters, as many as a REST path has.
inline constexpr std::size_t max_expression_query = 32;
inline constexpr std::size_t max_expression_path = 16;

// What a route's expression says, read once: its counts, its flags, where its query's keys are.
struct expression_summary {
  http::detail::template_error error = http::detail::template_error::none;
  std::size_t path_count = 0;
  std::size_t query_count = 0;
  bool typed_in_path = false;
  bool has_fixed_value = false;
  bool plain_keys = true;
  bool rest_of_path = false;
  std::array<std::size_t, max_expression_query> key_begin{};
  std::array<std::size_t, max_expression_query> key_size{};
  // Where the name of each `{parameter}` is, the path's then the query's: what a call by name names.
  std::array<std::size_t, max_expression_path + max_expression_query> name_begin{};
  std::array<std::size_t, max_expression_path + max_expression_query> name_size{};
};

[[nodiscard]] consteval expression_summary summarize(std::string_view text) {
  auto const parsed = http::detail::parse_template(text);
  expression_summary summary;

  summary.error = parsed.error;
  summary.query_count = parsed.query.size();

  for (auto const &segment : parsed.segments) {
    if (segment.kind != http::detail::segment_kind::literal && summary.path_count < max_expression_path) {
      summary.name_begin[summary.path_count] = static_cast<std::size_t>(segment.text.data() - text.data());
      summary.name_size[summary.path_count] = segment.text.size();
    }

    summary.path_count += segment.kind == http::detail::segment_kind::literal ? 0 : 1;
    summary.typed_in_path =
        summary.typed_in_path || (segment.kind == http::detail::segment_kind::param && !segment.type.empty());
  }

  summary.rest_of_path = !parsed.segments.empty() && parsed.segments.back().kind == http::detail::segment_kind::rest;

  for (std::size_t index = 0; index < parsed.query.size() && index < max_expression_query; ++index) {
    auto const &entry = parsed.query[index];

    summary.has_fixed_value = summary.has_fixed_value || entry.param.empty();
    summary.plain_keys = summary.plain_keys && entry.key.find_first_of("%+") == std::string_view::npos;
    summary.key_begin[index] = static_cast<std::size_t>(entry.key.data() - text.data());
    summary.key_size[index] = entry.key.size();

    if (summary.path_count <= max_expression_path && !entry.param.empty()) {
      summary.name_begin[summary.path_count + index] = static_cast<std::size_t>(entry.param.data() - text.data());
      summary.name_size[summary.path_count + index] = entry.param.size();
    }
  }

  return summary;
}

// What a route's expression says, `"/notes/{id}?fields={fields}"`: its path, before '?', and its query, read once at
// compile time by the router's grammar (http::detail::parse_template).
template <fixed_string Expr>
struct expression {
  [[nodiscard]] static consteval std::size_t find_question() {
    auto const at = http::detail::find_char(Expr.view(), '?');

    return at == std::string_view::npos ? Expr.view().size() : at;
  }

  static constexpr std::size_t question = find_question();
  static constexpr auto path = slice_string<Expr, 0, question>();
  static constexpr expression_summary info = summarize(Expr.view());

  static_assert(
      info.query_count <= max_expression_query, "quoll::rest: a route's expression has 32 query parameters at most"
  );
  static_assert(
      info.path_count <= max_expression_path, "quoll::rest: a route's expression has 16 path parameters at most"
  );

  [[nodiscard]] static consteval http::detail::template_error error() { return info.error; }

  [[nodiscard]] static consteval std::size_t path_count() { return info.path_count; }

  [[nodiscard]] static consteval std::size_t query_count() { return info.query_count; }

  [[nodiscard]] static consteval bool typed_in_path() { return info.typed_in_path; }

  [[nodiscard]] static consteval bool rest_of_path() { return info.rest_of_path; }

  [[nodiscard]] static consteval bool has_fixed_value() { return info.has_fixed_value; }

  [[nodiscard]] static consteval bool plain_keys() { return info.plain_keys; }

  [[nodiscard]] static consteval std::size_t key_begin(std::size_t index) { return info.key_begin[index]; }

  [[nodiscard]] static consteval std::size_t key_size(std::size_t index) { return info.key_size[index]; }

  // The name of its `Index`th `{parameter}`, the path's then the query's: a view of `Expr`, a template argument, which
  // lives as long as the program.
  [[nodiscard]] static consteval std::string_view parameter_name(std::size_t index) {
    return Expr.view().substr(info.name_begin[index], info.name_size[index]);
  }
};

// The key of the `Index`th pair of the query of `Expr`, as a fixed_string: the name of its query<> part.
template <fixed_string Expr, std::size_t Index>
inline constexpr auto query_key =
    slice_string<Expr, expression<Expr>::key_begin(Index), expression<Expr>::key_size(Index)>();

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_EXPRESSION_HPP
