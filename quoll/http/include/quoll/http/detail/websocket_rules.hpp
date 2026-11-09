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

#ifndef QUOLL_HTTP_DETAIL_WEBSOCKET_RULES_HPP
#define QUOLL_HTTP_DETAIL_WEBSOCKET_RULES_HPP

#include <quoll/http/websocket_message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace quoll::http::detail {

// What RFC 6455 asks of frames, which what reads them and what writes them share.

// The most a control frame (close, ping, pong) holds (5.5).
inline constexpr std::size_t max_control_payload = 125;

[[nodiscard]] constexpr bool is_known_opcode(std::uint8_t opcode) noexcept {
  return opcode <= 0x2 || (opcode >= 0x8 && opcode <= 0xA);
}

[[nodiscard]] constexpr bool is_control(websocket_opcode opcode) noexcept {
  return static_cast<std::uint8_t>(opcode) >= 0x8;
}

// The codes a peer may send in a close (7.4): not 1004, 1005, 1006 or 1015, nor what no one has named.
[[nodiscard]] constexpr bool may_send_close_code(std::uint16_t code) noexcept {
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

// XORs `bytes` with `key` (5.3), the first of them the `offset`th of the payload: masking and unmasking alike.
void apply_mask(std::span<char> bytes, std::array<std::uint8_t, 4> const &key, std::size_t offset = 0) noexcept;

// What a close's payload is (5.5.1): none, or a code a peer may send and a reason in UTF-8.
enum class close_payload : std::uint8_t { valid, broken, not_text };

[[nodiscard]] close_payload check_close_payload(std::string_view payload) noexcept;

// The code a close's payload of two bytes or more starts with.
[[nodiscard]] constexpr std::uint16_t close_code_in(std::string_view payload) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8U) |
                                    static_cast<std::uint8_t>(payload[1]));
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_WEBSOCKET_RULES_HPP
