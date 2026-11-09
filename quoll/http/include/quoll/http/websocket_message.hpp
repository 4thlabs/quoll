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

#ifndef QUOLL_HTTP_WEBSOCKET_MESSAGE_HPP
#define QUOLL_HTTP_WEBSOCKET_MESSAGE_HPP

#include <cstdint>
#include <string>

namespace quoll::http {

/** @brief The opcode of a WebSocket frame (RFC 6455, 5.2). */
enum class websocket_opcode : std::uint8_t {
  continuation = 0x0,
  text = 0x1,
  binary = 0x2,
  close = 0x8,
  ping = 0x9,
  pong = 0xA,
};

/**
 * @brief What a WebSocket peer sent, as http::websocket_parser reads it: a text or a binary message, its fragments
 * joined, a ping, a pong, or a close with its code and reason.
 */
struct websocket_message {
  /** @brief text, binary, close, ping or pong: never continuation. */
  websocket_opcode opcode = websocket_opcode::text;
  /** @brief The message's data: UTF-8 for text, the reason of a close. */
  std::string data;
  /** @brief The code of a close, 1005 (no status) if it had none; 0 for any other message. */
  std::uint16_t close_code = 0;

  [[nodiscard]] bool operator==(websocket_message const &) const = default;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_WEBSOCKET_MESSAGE_HPP
