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

#ifndef QUOLL_HTTP_DETAIL_PERCENT_HPP
#define QUOLL_HTTP_DETAIL_PERCENT_HPP

#include <quoll/http/detail/ascii.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::http::detail {

// Decodes the %XX escapes of a URL component. Nothing for a '%' not followed by two hex digits.
[[nodiscard]] inline std::optional<std::string> percent_decode(std::string_view text) {
  std::string decoded;

  decoded.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    auto const character = text[index];

    if (character != '%') {
      decoded += character;
      continue;
    }

    if (text.size() - index < 3) {
      return std::nullopt;
    }

    auto const high = hex_value(text[index + 1]);
    auto const low = hex_value(text[index + 2]);

    if (high < 0 || low < 0) {
      return std::nullopt;
    }

    decoded += static_cast<char>(high * 16 + low);
    index += 2;
  }

  return decoded;
}

// Characters a URL component never needs to escape (RFC 3986 "unreserved").
[[nodiscard]] constexpr bool is_unreserved(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
         character == '~';
}

// Escapes every character of `text` but the unreserved ones and those of `keep` (such as '/' for the rest of a path)
// as %XX.
[[nodiscard]] inline std::string percent_encode(std::string_view text, std::string_view keep = {}) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string encoded;

  encoded.reserve(text.size());

  for (auto const character : text) {
    if (is_unreserved(character) || keep.find(character) != std::string_view::npos) {
      encoded += character;
      continue;
    }

    auto const byte = static_cast<unsigned char>(character);

    encoded += '%';
    encoded += digits[byte >> 4U];
    encoded += digits[byte & 0x0FU];
  }

  return encoded;
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_PERCENT_HPP
