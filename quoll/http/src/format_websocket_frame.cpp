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

#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/detail/websocket_rules.hpp>
#include <quoll/http/format_websocket_frame.hpp>

#include <cstddef>
#include <stdexcept>

namespace quoll::http {

std::string format_websocket_frame(websocket_opcode opcode, std::string_view payload, bool fin,
                                   std::optional<websocket_mask> mask) {
  if (!detail::is_known_opcode(static_cast<std::uint8_t>(opcode))) {
    throw std::invalid_argument{"quoll::http::format_websocket_frame: an opcode RFC 6455 does not name"};
  }

  // What a peer would read as a close it must refuse: a byte alone, a code no one sends, a reason not UTF-8.
  if (opcode == websocket_opcode::close && detail::check_close_payload(payload) != detail::close_payload::valid) {
    throw std::invalid_argument{"quoll::http::format_websocket_frame: a close a peer would refuse"};
  }

  if (detail::is_control(opcode) && (!fin || payload.size() > detail::max_control_payload)) {
    throw std::invalid_argument{"quoll::http::format_websocket_frame: a control frame is whole, of 125 bytes at most"};
  }

  std::string out;

  out.reserve(payload.size() + 14);
  out.push_back(static_cast<char>((fin ? 0x80U : 0U) | static_cast<std::uint8_t>(opcode)));

  auto const masked = mask ? 0x80U : 0U;

  if (payload.size() < 126) {
    out.push_back(static_cast<char>(masked | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    out.push_back(static_cast<char>(masked | 126U));
    out.push_back(static_cast<char>(payload.size() >> 8U));
    out.push_back(static_cast<char>(payload.size() & 0xFFU));
  } else {
    auto const length = static_cast<std::uint64_t>(payload.size());

    out.push_back(static_cast<char>(masked | 127U));

    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((length >> static_cast<unsigned>(shift)) & 0xFFU));
    }
  }

  if (!mask) {
    out.append(payload);
    return out;
  }

  for (auto const key : *mask) {
    out.push_back(static_cast<char>(key));
  }

  for (std::size_t index = 0; index < payload.size(); ++index) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ (*mask)[index % 4]));
  }

  return out;
}

std::string format_websocket_close(std::uint16_t code, std::string_view reason, std::optional<websocket_mask> mask) {
  if (!detail::may_send_close_code(code)) {
    throw std::invalid_argument{"quoll::http::format_websocket_close: a code no one may send"};
  }

  if (reason.size() > 123 || !detail::is_utf8(reason)) {
    throw std::invalid_argument{"quoll::http::format_websocket_close: a reason is UTF-8, of 123 bytes at most"};
  }

  std::string payload;

  payload.push_back(static_cast<char>(code >> 8U));
  payload.push_back(static_cast<char>(code & 0xFFU));
  payload.append(reason);
  return format_websocket_frame(websocket_opcode::close, payload, true, mask);
}

} // namespace quoll::http
