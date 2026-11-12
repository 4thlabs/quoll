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

#ifndef QUOLL_HTTP_DETAIL_ROUTE_TEMPLATE_HPP
#define QUOLL_HTTP_DETAIL_ROUTE_TEMPLATE_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/plain_segment.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::http::detail {

// In order of precedence: a literal segment beats a parameter, which beats the rest of the path.
enum class segment_kind { rest, param, literal };

// What makes a route template invalid.
enum class template_error {
  none,
  no_leading_slash,
  unbalanced_brace,
  invalid_name,
  not_whole_segment,
  rest_not_last,
  duplicate_name,
  invalid_escape,
  dot_segment,
  empty_type,
};

struct template_segment {
  segment_kind kind = segment_kind::literal;
  // The literal as written, or the parameter name.
  std::string_view text;
  // The `type` of `{name:type}`: empty for `{name}` and for literals, "path" for the rest of the path.
  std::string_view type;
};

struct parsed_template {
  template_error error = template_error::none;
  std::vector<template_segment> segments;
};

// string_view::find, by index: GCC 13 cannot evaluate the null check of char_traits::find at compile time on a
// template parameter object (an http::fixed_string) when UBSan instruments the code, and this grammar runs at compile
// time for the rest DSL.
[[nodiscard]] constexpr std::size_t find_char(std::string_view text, char character, std::size_t from = 0) noexcept {
  for (auto index = from; index < text.size(); ++index) {
    if (text[index] == character) {
      return index;
    }
  }

  return std::string_view::npos;
}

[[nodiscard]] constexpr bool is_name_char(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_';
}

// Whether every '%' starts a %XX escape.
[[nodiscard]] constexpr bool valid_escapes(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%') {
      continue;
    }

    if (text.size() - index < 3 || !is_hex_digit(text[index + 1]) || !is_hex_digit(text[index + 2])) {
      return false;
    }

    index += 2;
  }

  return true;
}

// What http::detail::plain_segment says of a literal with valid escapes, at compile time too: no request could reach
// one that fails, as what the router sees of it is refused.
// A literal whose escapes are valid, decoded: what the router compares, at compile time too.
[[nodiscard]] constexpr std::string decoded_literal(std::string_view literal) {
  std::string decoded;

  for (std::size_t index = 0; index < literal.size(); ++index) {
    if (literal[index] != '%') {
      decoded += literal[index];
      continue;
    }

    decoded += static_cast<char>(hex_value(literal[index + 1]) * 16 + hex_value(literal[index + 2]));
    index += 2;
  }

  return decoded;
}

[[nodiscard]] constexpr bool plain_literal(std::string_view text) noexcept {
  plain_text decoded;

  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%') {
      decoded.feed(static_cast<char>(hex_value(text[index + 1]) * 16 + hex_value(text[index + 2])));
      index += 2;
    } else {
      decoded.feed(text[index]);
    }
  }

  return decoded.plain();
}

[[nodiscard]] constexpr template_error check_parameter(std::string_view segment) noexcept {
  auto const open = find_char(segment, '{');
  auto const close = find_char(segment, '}');

  if (open == std::string_view::npos || close == std::string_view::npos || close < open ||
      find_char(segment, '{', open + 1) != std::string_view::npos ||
      find_char(segment, '}', close + 1) != std::string_view::npos) {
    return template_error::unbalanced_brace;
  }

  if (open != 0 || close != segment.size() - 1) {
    return template_error::not_whole_segment;
  }

  return template_error::none;
}

// The grammar of route templates, shared by http::router (at run time) and the rest DSL (at compile time):
// `/users/{id}`, `{name:type}`, `{name:path}` for the rest of the path, last. A parameter takes a whole segment and
// has a name of letters, digits and '_', unique in the template. Literals may hold %XX escapes. "" has no segment
// (the root of an api prefix), "/" one empty literal.
[[nodiscard]] constexpr parsed_template parse_template(std::string_view path) {
  parsed_template parsed;

  auto fail = [&parsed](template_error error) {
    parsed.error = error;
    parsed.segments.clear();
    return parsed;
  };

  if (path.empty()) {
    return parsed;
  }

  if (path.front() != '/') {
    return fail(template_error::no_leading_slash);
  }

  for (std::size_t start = 1;;) {
    auto const slash = find_char(path, '/', start);
    auto const segment = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);

    if (!parsed.segments.empty() && parsed.segments.back().kind == segment_kind::rest) {
      return fail(template_error::rest_not_last);
    }

    if (segment.find_first_of("{}") == std::string_view::npos) {
      if (!valid_escapes(segment)) {
        return fail(template_error::invalid_escape);
      }

      if (!plain_literal(segment)) {
        return fail(template_error::dot_segment);
      }

      parsed.segments.push_back({segment_kind::literal, segment, {}});
    } else {
      if (auto const error = check_parameter(segment); error != template_error::none) {
        return fail(error);
      }

      auto const inside = segment.substr(1, segment.size() - 2);
      auto const colon = find_char(inside, ':');
      auto const name = inside.substr(0, colon);
      auto const type = colon == std::string_view::npos ? std::string_view{} : inside.substr(colon + 1);

      if (name.empty() || !std::ranges::all_of(name, is_name_char)) {
        return fail(template_error::invalid_name);
      }

      if (colon != std::string_view::npos && type.empty()) {
        return fail(template_error::empty_type);
      }

      auto const same_name = [name](template_segment const &other) {
        return other.kind != segment_kind::literal && other.text == name;
      };

      if (std::ranges::any_of(parsed.segments, same_name)) {
        return fail(template_error::duplicate_name);
      }

      parsed.segments.push_back({type == "path" ? segment_kind::rest : segment_kind::param, name, type});
    }

    if (slash == std::string_view::npos) {
      return parsed;
    }

    start = slash + 1;
  }
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ROUTE_TEMPLATE_HPP
