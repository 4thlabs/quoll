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

#include <quoll/http/detail/query_string.hpp>

#include <quoll/http/detail/percent.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http::detail {

std::optional<std::string> decode_query_component(std::string_view text) {
  std::string spaced{text};

  std::ranges::replace(spaced, '+', ' ');
  return http::detail::percent_decode(spaced);
}

namespace {

[[nodiscard]] std::optional<std::string> decoded_part(std::string_view text) {
  auto decoded = decode_query_component(text);

  if (!decoded || decoded->contains('\0')) {
    return std::nullopt;
  }

  return decoded;
}

} // namespace

std::vector<query_pair> query_pairs(std::string_view query) {
  std::vector<query_pair> pairs;

  for (auto const piece : std::views::split(query, '&')) {
    std::string_view const text{piece.begin(), piece.end()};

    if (text.empty()) {
      continue;
    }

    auto const equals = text.find('=');

    pairs.push_back(
        {decoded_part(text.substr(0, equals)),
         decoded_part(equals == std::string_view::npos ? std::string_view{} : text.substr(equals + 1))}
    );
  }

  return pairs;
}

std::optional<std::vector<query_entry>> parse_query(std::string_view query) {
  std::vector<query_entry> entries;

  for (auto &pair : query_pairs(query)) {
    if (!pair.name || !pair.value) {
      return std::nullopt;
    }

    entries.push_back({std::move(*pair.name), std::move(*pair.value)});
  }

  return entries;
}

std::vector<std::string_view> query_texts(std::vector<query_entry> const &query, std::string_view name) {
  std::vector<std::string_view> texts;

  for (auto const &entry : query) {
    if (entry.name == name) {
      texts.push_back(entry.value);
    }
  }

  return texts;
}

void append_query(std::string &query, std::string_view name, std::string_view value, std::string_view what) {
  if (value.contains('\0')) {
    throw std::invalid_argument{std::string{what} + " " + std::string{name} + " holds a NUL, which the server refuses"};
  }

  query += query.empty() ? "" : "&";
  query += http::detail::percent_encode(name);
  query += '=';
  query += http::detail::percent_encode(value);
}

} // namespace quoll::http::detail
