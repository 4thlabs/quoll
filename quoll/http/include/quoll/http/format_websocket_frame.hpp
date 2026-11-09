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

#ifndef QUOLL_HTTP_FORMAT_WEBSOCKET_FRAME_HPP
#define QUOLL_HTTP_FORMAT_WEBSOCKET_FRAME_HPP

#include <quoll/http/websocket_message.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::http {

/** @brief The key a client masks a frame with (RFC 6455, 5.3): four bytes, from a strong source of randomness. */
using websocket_mask = std::array<std::uint8_t, 4>;

/**
 * @brief A WebSocket frame of `payload`, in the shortest form of its length: masked with `mask` if given, as a client
 * sends it, not as a server does. `fin` false makes it a fragment, continued by frames of `continuation`.
 *
 * Text is not checked here, as a fragment may end in the middle of a character: what sends a message checks it.
 *
 * @throws std::invalid_argument for an opcode RFC 6455 does not name, a control frame (close, ping, pong) that is a
 * fragment or longer than 125 bytes, or a close a peer would refuse: of one byte, with a code no one may send, or a
 * reason that is not UTF-8.
 */
[[nodiscard]] std::string format_websocket_frame(websocket_opcode opcode, std::string_view payload, bool fin = true,
                                                 std::optional<websocket_mask> mask = std::nullopt);

/**
 * @brief A close frame with `code` and `reason`, masked with `mask` if given.
 *
 * @throws std::invalid_argument for a code no one may send (1004 to 1006, 1015, under 1000, 1016 to 2999, 5000 and
 * above), a reason that is not UTF-8, or longer than 123 bytes.
 */
[[nodiscard]] std::string format_websocket_close(std::uint16_t code, std::string_view reason = {},
                                                 std::optional<websocket_mask> mask = std::nullopt);

} // namespace quoll::http

#endif // QUOLL_HTTP_FORMAT_WEBSOCKET_FRAME_HPP
