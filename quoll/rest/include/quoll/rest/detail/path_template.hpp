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

template <param_kind Kind>
struct param_kind_type;

template <>
struct param_kind_type<param_kind::int32> {
  using type = int;
};

template <>
struct param_kind_type<param_kind::int64> {
  using type = std::int64_t;
};

template <>
struct param_kind_type<param_kind::uint32> {
  using type = unsigned;
};

template <>
struct param_kind_type<param_kind::uint64> {
  using type = std::uint64_t;
};

template <>
struct param_kind_type<param_kind::floating> {
  using type = double;
};

template <>
struct param_kind_type<param_kind::boolean> {
  using type = bool;
};

template <>
struct param_kind_type<param_kind::string> {
  using type = std::string;
};

template <>
struct param_kind_type<param_kind::rest_of_path> {
  using type = std::string;
};

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

[[nodiscard]] consteval bool is_name_char(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_';
}

[[nodiscard]] consteval bool parse_kind(std::string_view text, param_kind &kind) {
  constexpr std::array<std::pair<std::string_view, param_kind>, 8> kinds{{
      {"int", param_kind::int32},
      {"int64", param_kind::int64},
      {"uint", param_kind::uint32},
      {"uint64", param_kind::uint64},
      {"double", param_kind::floating},
      {"bool", param_kind::boolean},
      {"string", param_kind::string},
      {"path", param_kind::rest_of_path},
  }};

  for (auto const &[spelling, value] : kinds) {
    if (spelling == text) {
      kind = value;
      return true;
    }
  }

  return false;
}

// Parses `{name}` (a string) and `{name:type}` segments. A parameter takes a whole segment; `path`, the rest of the
// path, comes last. An empty template is valid (the root of an api).
[[nodiscard]] consteval path_template parse_path(std::string_view text) {
  path_template parsed;

  auto fail = [&parsed](path_error error) {
    parsed.error = error;
    return parsed;
  };

  if (!text.empty() && text.front() != '/') {
    return fail(path_error::no_leading_slash);
  }

  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '}') {
      return fail(path_error::unbalanced_brace);
    }

    if (text[index] != '{') {
      continue;
    }

    auto const close = text.find('}', index);

    if (close == std::string_view::npos ||
        text.substr(index + 1, close - index - 1).find('{') != std::string_view::npos) {
      return fail(path_error::unbalanced_brace);
    }

    if (text[index - 1] != '/' || (close + 1 < text.size() && text[close + 1] != '/')) {
      return fail(path_error::not_whole_segment);
    }

    auto const inside = text.substr(index + 1, close - index - 1);
    auto const colon = inside.find(':');
    auto const name = inside.substr(0, colon);
    path_param param{name, param_kind::string};

    if (name.empty() || !std::ranges::all_of(name, is_name_char)) {
      return fail(path_error::invalid_name);
    }

    if (colon != std::string_view::npos && !parse_kind(inside.substr(colon + 1), param.kind)) {
      return fail(path_error::unknown_type);
    }

    for (std::size_t other = 0; other < parsed.size; ++other) {
      if (parsed.params[other].name == name) {
        return fail(path_error::duplicate_name);
      }
    }

    if (parsed.size == max_path_params) {
      return fail(path_error::too_many_parameters);
    }

    // The rest of the path: nothing may follow, neither a parameter nor a literal segment.
    if (param.kind == param_kind::rest_of_path && close + 1 < text.size()) {
      return fail(path_error::rest_not_last);
    }

    parsed.params[parsed.size++] = param;
    index = close;
  }

  return parsed;
}

// The path of an endpoint under the prefix of its api: an endpoint path of "/" is the prefix itself, and an empty
// result is "/".
template <fixed_string Prefix, fixed_string Path>
[[nodiscard]] consteval auto join_paths() {
  constexpr std::string_view prefix = Prefix.view();
  constexpr std::string_view tail = Path.view() == "/" ? std::string_view{} : Path.view();
  constexpr std::size_t length = prefix.size() + tail.size() == 0 ? 1 : prefix.size() + tail.size();

  fixed_string<length + 1> joined;

  if constexpr (prefix.size() + tail.size() == 0) {
    joined.value[0] = '/';
  } else {
    std::ranges::copy(prefix, joined.value);
    std::ranges::copy(tail, joined.value + prefix.size());
  }

  return joined;
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PATH_TEMPLATE_HPP
