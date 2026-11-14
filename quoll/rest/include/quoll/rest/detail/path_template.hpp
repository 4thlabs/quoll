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

#ifndef QUOLL_REST_DETAIL_PATH_TEMPLATE_HPP
#define QUOLL_REST_DETAIL_PATH_TEMPLATE_HPP

#include <quoll/http/detail/route_template.hpp>
#include <quoll/rest/detail/join_strings.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest::detail {

// The types a path parameter may have, spelled `{name:type}` in a path template.
enum class param_kind { int32, int64, uint32, uint64, floating, boolean, string, rest_of_path };

// What makes a path template invalid; each one has its own static_assert message.
enum class path_error {
  none,
  no_leading_slash,
  unbalanced_brace,
  invalid_name,
  unknown_type,
  not_whole_segment,
  rest_not_last,
  duplicate_name,
  invalid_escape,
  dot_segment,
  invalid_character,
  too_many_parameters,
};

struct path_param {
  std::string_view name;
  param_kind kind = param_kind::string;
};

inline constexpr std::size_t max_path_params = 16;

struct path_template {
  path_error error = path_error::none;
  std::size_t size = 0;
  std::array<path_param, max_path_params> params{};
};

[[nodiscard]] consteval bool parse_kind(std::string_view text, param_kind &kind) {
  constexpr std::array<std::pair<std::string_view, param_kind>, 7> kinds{{
      {"int", param_kind::int32},
      {"int64", param_kind::int64},
      {"uint", param_kind::uint32},
      {"uint64", param_kind::uint64},
      {"double", param_kind::floating},
      {"bool", param_kind::boolean},
      {"string", param_kind::string},
  }};

  for (auto const &[spelling, value] : kinds) {
    if (spelling == text) {
      kind = value;
      return true;
    }
  }

  return false;
}

[[nodiscard]] consteval path_error to_path_error(http::detail::template_error error) {
  using http::detail::template_error;

  switch (error) {
  case template_error::none:
    break;
  case template_error::no_leading_slash:
    return path_error::no_leading_slash;
  case template_error::unbalanced_brace:
    return path_error::unbalanced_brace;
  case template_error::invalid_name:
    return path_error::invalid_name;
  case template_error::not_whole_segment:
    return path_error::not_whole_segment;
  case template_error::rest_not_last:
    return path_error::rest_not_last;
  case template_error::duplicate_name:
    return path_error::duplicate_name;
  case template_error::invalid_escape:
    return path_error::invalid_escape;
  case template_error::dot_segment:
    return path_error::dot_segment;
  case template_error::invalid_query:
  case template_error::duplicate_query_key:
    return path_error::invalid_character;
  case template_error::empty_type:
    return path_error::unknown_type;
  }

  return path_error::none;
}

// What a literal of a route's path may hold (RFC 3986, pchar): a client copies it into the target as it is, where a
// space, '?', '#', a control or a byte past ASCII would take the request elsewhere, or none could carry it. The router
// itself matches literals decoded, and takes them.
[[nodiscard]] constexpr bool is_path_character(char character) noexcept {
  constexpr std::string_view others = "-._~!$&'()*+,;=:@%";

  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || http::detail::find_char(others, character) != std::string_view::npos;
}

// The grammar is the router's (http::detail::parse_template); the DSL adds the parameter types: `{name}` is a string,
// `{name:type}` one of the types above, `{name:path}` the rest of the path. An empty template is valid (the root of
// an api).
[[nodiscard]] consteval path_template parse_path(std::string_view text) {
  path_template parsed;

  // The path of a route, without its query, which its expression holds after the '?': here, '?' is no character of it.
  if (http::detail::find_char(text, '?') != std::string_view::npos) {
    parsed.error = path_error::invalid_character;
    return parsed;
  }

  auto const grammar = http::detail::parse_template(text);

  parsed.error = to_path_error(grammar.error);

  for (auto const &segment : grammar.segments) {
    if (segment.kind == http::detail::segment_kind::literal) {
      if (!std::ranges::all_of(segment.text, is_path_character)) {
        parsed.error = path_error::invalid_character;
        return parsed;
      }

      continue;
    }

    path_param param{segment.text, param_kind::rest_of_path};

    if (segment.kind == http::detail::segment_kind::param && !segment.type.empty() &&
        !parse_kind(segment.type, param.kind)) {
      parsed.error = path_error::unknown_type;
      return parsed;
    }

    if (segment.kind == http::detail::segment_kind::param && segment.type.empty()) {
      param.kind = param_kind::string;
    }

    if (parsed.size == max_path_params) {
      parsed.error = path_error::too_many_parameters;
      return parsed;
    }

    parsed.params[parsed.size++] = param;
  }

  return parsed;
}

// The path of a route under the prefix of its api: a route path of "/" is the prefix itself, and an empty
// result is "/".
template <fixed_string Prefix, fixed_string Path>
[[nodiscard]] consteval auto join_paths() {
  constexpr bool root = Path.view() == "/" || Path.view().empty();

  if constexpr (root && Prefix.view().empty()) {
    return fixed_string{"/"};
  } else if constexpr (root) {
    return Prefix;
  } else {
    return join_strings<Prefix, Path>();
  }
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PATH_TEMPLATE_HPP
