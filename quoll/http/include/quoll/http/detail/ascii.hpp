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

#ifndef QUOLL_HTTP_DETAIL_ASCII_HPP
#define QUOLL_HTTP_DETAIL_ASCII_HPP

#include <cstddef>
#include <string_view>

namespace quoll::http::detail {

// HTTP is ASCII on the wire: locale-free helpers, unlike <cctype>.

[[nodiscard]] constexpr char to_lower(char character) noexcept {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
}

[[nodiscard]] constexpr bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (to_lower(lhs[index]) != to_lower(rhs[index])) {
      return false;
    }
  }

  return true;
}

// tchar of RFC 9110: the characters allowed in a field name or a method.
[[nodiscard]] constexpr bool is_token_char(char character) noexcept {
  if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
      (character >= '0' && character <= '9')) {
    return true;
  }

  return std::string_view{"!#$%&'*+-.^_`|~"}.find(character) != std::string_view::npos;
}

[[nodiscard]] constexpr bool is_token(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }

  for (char const character : text) {
    if (!is_token_char(character)) {
      return false;
    }
  }

  return true;
}

// What a field value, a reason phrase or a request target must never contain: it would end the line early.
[[nodiscard]] constexpr bool has_line_break(std::string_view text) noexcept {
  return text.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos;
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  auto const first = text.find_first_not_of(" \t");

  if (first == std::string_view::npos) {
    return {};
  }

  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ASCII_HPP
