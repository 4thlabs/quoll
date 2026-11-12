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

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/websocket_key.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/http/websocket_handshake.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace quoll::http {

namespace {

constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// SHA-1 (RFC 3174): what RFC 6455 asks for, not for any security of its own.
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view text) {
  std::array<std::uint32_t, 5> state{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
  std::string message{text};
  auto const bits = static_cast<std::uint64_t>(text.size()) * 8U;

  message.push_back(static_cast<char>(0x80));

  while (message.size() % 64 != 56) {
    message.push_back('\0');
  }

  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<char>((bits >> static_cast<unsigned>(shift)) & 0xFFU));
  }

  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 80> words{};

    for (std::size_t index = 0; index < 16; ++index) {
      for (std::size_t part = 0; part < 4; ++part) {
        words[index] = (words[index] << 8U) | static_cast<std::uint8_t>(message[block + (index * 4) + part]);
      }
    }

    for (std::size_t index = 16; index < 80; ++index) {
      words[index] = std::rotl(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
    }

    auto [a, b, c, d, e] = state;

    for (std::size_t index = 0; index < 80; ++index) {
      std::uint32_t mixed = 0;
      std::uint32_t constant = 0;

      if (index < 20) {
        mixed = (b & c) | (~b & d);
        constant = 0x5A827999U;
      } else if (index < 40) {
        mixed = b ^ c ^ d;
        constant = 0x6ED9EBA1U;
      } else if (index < 60) {
        mixed = (b & c) | (b & d) | (c & d);
        constant = 0x8F1BBCDCU;
      } else {
        mixed = b ^ c ^ d;
        constant = 0xCA62C1D6U;
      }

      auto const next = std::rotl(a, 5) + mixed + e + constant + words[index];

      e = d;
      d = c;
      c = std::rotl(b, 30);
      b = a;
      a = next;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
  }

  std::array<std::uint8_t, 20> digest{};

  for (std::size_t index = 0; index < 20; ++index) {
    digest[index] = static_cast<std::uint8_t>(state[index / 4] >> (24U - (8U * (index % 4))));
  }

  return digest;
}

[[nodiscard]] std::string base64(std::span<std::uint8_t const> bytes) {
  std::string out;

  for (std::size_t index = 0; index < bytes.size(); index += 3) {
    std::uint32_t group = std::uint32_t{bytes[index]} << 16U;
    auto const left = bytes.size() - index;

    if (left > 1) {
      group |= std::uint32_t{bytes[index + 1]} << 8U;
    }

    if (left > 2) {
      group |= bytes[index + 2];
    }

    out.push_back(base64_digits[(group >> 18U) & 0x3FU]);
    out.push_back(base64_digits[(group >> 12U) & 0x3FU]);
    out.push_back(left > 1 ? base64_digits[(group >> 6U) & 0x3FU] : '=');
    out.push_back(left > 2 ? base64_digits[group & 0x3FU] : '=');
  }

  return out;
}

// A key of 16 bytes in base64: 22 digits, the last with its low four bits clear (A, Q, g or w), then `==`.
[[nodiscard]] bool is_key(std::string_view key) {
  if (key.size() != 24 || !key.ends_with("==") || !std::string_view{"AQgw"}.contains(key[21])) {
    return false;
  }

  return std::ranges::all_of(key.substr(0, 22), [](char digit) { return base64_digits.contains(digit); });
}

} // namespace

bool is_websocket_upgrade(request const &message) noexcept {
  // One key and one version: two would leave which one counts to whoever reads them.
  auto const once = [&message](std::string_view name) {
    return std::ranges::count_if(message.fields, [name](auto const &entry) {
             return detail::iequals(entry.name, name);
           }) == 1;
  };

  return message.method == verb::get && message.version == version::http_1_1 && message.fields.contains("Host") &&
         once("Sec-WebSocket-Key") && once("Sec-WebSocket-Version") &&
         detail::field_names(message.fields, "Upgrade", "websocket") &&
         detail::connection_names(message.fields, "upgrade") &&
         detail::trim(message.fields.find("Sec-WebSocket-Version").value_or("")) == "13" &&
         is_key(detail::trim(message.fields.find("Sec-WebSocket-Key").value_or("")));
}

namespace detail {

std::string random_websocket_key() {
  std::random_device source;
  std::array<std::uint8_t, 16> bytes{};

  for (std::size_t index = 0; index < bytes.size(); index += 4) {
    auto const bits = static_cast<std::uint32_t>(source());

    for (std::size_t shift = 0; shift < 4; ++shift) {
      bytes.at(index + shift) = static_cast<std::uint8_t>(bits >> (8U * shift));
    }
  }

  return base64(bytes);
}

} // namespace detail

std::string websocket_accept(std::string_view key) {
  // Trimmed as is_websocket_upgrade reads it: the key the client sent, not the spaces around it.
  std::string joined{detail::trim(key)};

  joined.append(guid);

  auto const digest = sha1(joined);

  return base64(digest);
}

} // namespace quoll::http
