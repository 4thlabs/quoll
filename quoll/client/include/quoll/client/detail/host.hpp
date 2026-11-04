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

#ifndef QUOLL_CLIENT_DETAIL_HOST_HPP
#define QUOLL_CLIENT_DETAIL_HOST_HPP

#include <quoll/http/detail/ascii.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::client::detail {

// The position of `character` in `text` from `from`, or the size of `text`.
[[nodiscard]] constexpr std::size_t end_of(std::string_view text, char character, std::size_t from) noexcept {
  while (from < text.size() && text[from] != character) {
    ++from;
  }

  return from;
}

// A host name or an IPv4 address: letters, digits, '-', '_' and the dots between labels.
[[nodiscard]] constexpr bool valid_host_name(std::string_view host) noexcept {
  if (host.empty()) {
    return false;
  }

  for (auto const character : host) {
    auto const allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                         character == '.';

    if (!allowed) {
      return false;
    }
  }

  return true;
}

// Four decimal numbers up to 255, separated by dots.
[[nodiscard]] constexpr bool valid_ipv4(std::string_view text) noexcept {
  std::size_t index = 0;

  for (int part = 0; part < 4; ++part) {
    auto const end = end_of(text, '.', index);

    if (end == index || end - index > 3 || (part < 3) != (end < text.size())) {
      return false;
    }

    int value = 0;

    for (auto const character : text.substr(index, end - index)) {
      if (character < '0' || character > '9') {
        return false;
      }

      value = value * 10 + (character - '0');
    }

    if (value > 255) {
      return false;
    }

    index = end + 1;
  }

  return true;
}

// The text form of an IPv6 address (RFC 4291): eight groups of 1 to 4 hex digits separated by colons, one "::"
// standing for one or more groups of zeros, the last two groups possibly written as an IPv4 address. No zone index.
[[nodiscard]] constexpr bool valid_ipv6(std::string_view text) noexcept {
  std::size_t groups = 0;
  std::size_t index = 0;
  auto compressed = false;

  if (text.starts_with("::")) {
    compressed = true;
    index = 2;
  } else if (text.starts_with(':')) {
    return false;
  }

  while (index < text.size()) {
    auto const end = end_of(text, ':', index);
    auto const group = text.substr(index, end - index);

    if (end == text.size() && group.contains('.')) {
      if (!valid_ipv4(group)) {
        return false;
      }

      groups += 2;
      break;
    }

    if (group.empty() || group.size() > 4) {
      return false;
    }

    for (auto const character : group) {
      if (!http::detail::is_hex_digit(character)) {
        return false;
      }
    }

    ++groups;

    if (end == text.size()) {
      break;
    }

    index = end + 1;

    if (index == text.size()) {
      // A single trailing colon.
      return false;
    }

    if (text[index] == ':') {
      if (compressed) {
        return false;
      }

      compressed = true;
      ++index;
    }
  }

  return compressed ? groups < 8 : groups == 8;
}

// The Host field naming `host` on `port`: an IPv6 address in brackets, the port unless it is the scheme's default.
// Throws std::invalid_argument for a host that is neither a name, an IPv4 nor an IPv6 address.
[[nodiscard]] inline std::string host_field(std::string_view host, std::uint16_t port,
                                            std::uint16_t default_port = 80) {
  auto const ipv6 = host.contains(':');

  if (ipv6 ? !valid_ipv6(host) : !valid_host_name(host)) {
    throw std::invalid_argument{"quoll::client::transport: invalid host"};
  }

  auto field = ipv6 ? "[" + std::string{host} + "]" : std::string{host};

  if (port != default_port) {
    field += ':' + std::to_string(port);
  }

  return field;
}

} // namespace quoll::client::detail

#endif // QUOLL_CLIENT_DETAIL_HOST_HPP
