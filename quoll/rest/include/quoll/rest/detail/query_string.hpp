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

#ifndef QUOLL_REST_DETAIL_QUERY_STRING_HPP
#define QUOLL_REST_DETAIL_QUERY_STRING_HPP

#include <quoll/http/detail/percent.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

struct query_entry {
  std::string name;
  std::string value;
};

// Decodes a component of a query string: '+' is a space, as forms send it, and %XX escapes.
[[nodiscard]] inline std::optional<std::string> decode_query_component(std::string_view text) {
  std::string spaced{text};

  std::ranges::replace(spaced, '+', ' ');
  return http::detail::percent_decode(spaced);
}

// The `name=value` pairs of a query string, decoded and in order; `name` alone has an empty value. Nothing for an
// invalid escape.
[[nodiscard]] inline std::optional<std::vector<query_entry>> parse_query(std::string_view query) {
  std::vector<query_entry> entries;

  for (auto const pair : std::views::split(query, '&')) {
    std::string_view const text{pair.begin(), pair.end()};

    if (text.empty()) {
      continue;
    }

    auto const equals = text.find('=');
    auto name = decode_query_component(text.substr(0, equals));
    auto value =
        decode_query_component(equals == std::string_view::npos ? std::string_view{} : text.substr(equals + 1));

    if (!name || !value) {
      return std::nullopt;
    }

    entries.push_back({std::move(*name), std::move(*value)});
  }

  return entries;
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_QUERY_STRING_HPP
