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
#include <quoll/http/error.hpp>
#include <quoll/http/websocket_parser.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace quoll::http {

namespace {

// The capacity an empty buffer keeps: about what a socket gives at once.
constexpr std::size_t kept_capacity = std::size_t{64} * 1024;

[[nodiscard]] constexpr std::uint8_t byte(char value) noexcept { return static_cast<std::uint8_t>(value); }

} // namespace

void websocket_parser::feed(std::string_view bytes) {
  if (failure_) {
    throw std::system_error{failure_, "quoll::http::websocket_parser"};
  }

  // Nothing is read after a close: what still comes is dropped, whether next() was asked again or not.
  if (closed_) {
    return;
  }

  // Messages not taken yet: one piece after another, they would only grow.
  if (!drained_) {
    throw std::logic_error{"quoll::http::websocket_parser: fed before next() gave every message"};
  }

  buffer_.append(bytes);
  drained_ = false;
}

std::optional<websocket_message> websocket_parser::next() {
  if (failure_) {
    throw std::system_error{failure_, "quoll::http::websocket_parser"};
  }

  for (;;) {
    // Nothing follows a close: what came after it is dropped, and what it held goes.
    if (closed_) {
      buffer_ = {};
      data_ = {};
      position_ = 0;
      drained_ = true;
      return std::nullopt;
    }

    bool whole = false;
    auto message = frame(whole);

    if (!whole) {
      buffer_.erase(0, position_);
      position_ = 0;

      // A large message once read leaves no large buffer behind, for the life of a connection.
      if (buffer_.capacity() > std::max(kept_capacity, 2 * buffer_.size())) {
        buffer_.shrink_to_fit();
      }

      drained_ = true;
      return std::nullopt;
    }

    if (message) {
      return message;
    }
  }
}

std::optional<websocket_message> websocket_parser::frame(bool &whole) {
  auto const rest = std::string_view{buffer_}.substr(position_);

  if (rest.size() < 2) {
    return std::nullopt;
  }

  auto const first = byte(rest[0]);
  auto const second = byte(rest[1]);
  bool const fin = (first & 0x80U) != 0;
  auto const raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);

  // No extension is agreed: its bits stay clear.
  if ((first & 0x70U) != 0 || !detail::is_known_opcode(raw_opcode)) {
    fail(error::websocket_protocol);
  }

  auto const opcode = static_cast<websocket_opcode>(raw_opcode);
  bool const masked = (second & 0x80U) != 0;

  // A client masks what it sends, a server does not (RFC 6455, 5.1).
  if (masked != (role_ == websocket_role::server)) {
    fail(error::websocket_protocol);
  }

  std::uint64_t length = second & 0x7FU;
  std::size_t header = 2;

  if (length == 126) {
    if (rest.size() < 4) {
      return std::nullopt;
    }

    length = (std::uint64_t{byte(rest[2])} << 8U) | byte(rest[3]);
    header = 4;

    if (length < 126) {
      fail(error::websocket_protocol);
    }
  } else if (length == 127) {
    if (rest.size() < 10) {
      return std::nullopt;
    }

    length = 0;

    for (std::size_t index = 2; index < 10; ++index) {
      length = (length << 8U) | byte(rest[index]);
    }

    header = 10;

    // Its most significant bit clear, and in its shortest form.
    if ((length >> 63U) != 0 || length <= 0xFFFF) {
      fail(error::websocket_protocol);
    }
  }

  if (detail::is_control(opcode)) {
    if (!fin || length > detail::max_control_payload) {
      fail(error::websocket_protocol);
    }
  } else {
    if ((opcode == websocket_opcode::continuation) != continuing_.has_value()) {
      fail(error::websocket_protocol);
    }

    // Before its bytes are held: what the message would hold with them.
    auto const held = opcode == websocket_opcode::continuation ? data_.size() : 0;

    if (length > max_message_size_ - held) {
      fail(error::websocket_too_large);
    }
  }

  if (masked) {
    header += 4;
  }

  if (rest.size() < header || rest.size() - header < length) {
    return std::nullopt;
  }

  whole = true;

  auto const size = static_cast<std::size_t>(length);
  auto const key = masked ? rest.substr(header - 4, 4) : std::string_view{};

  // Unmasked as it is copied out of the buffer, into the message it continues for a data frame: no frame of its own.
  auto const take_into = [&](std::string &out) {
    auto const from = out.size();

    out.append(rest.substr(header, size));

    if (masked) {
      detail::apply_mask(std::span{out}.subspan(from), {byte(key[0]), byte(key[1]), byte(key[2]), byte(key[3])});
    }
  };

  if (!detail::is_control(opcode)) {
    if (opcode != websocket_opcode::continuation) {
      continuing_ = opcode;
      data_.clear();
      checked_ = 0;
    }

    take_into(data_);
    position_ += header + size;

    // Checked as each fragment comes, a character it cuts short carried to the next: a text that is not UTF-8 fails
    // at its first wrong byte, not once a whole message of it is held.
    if (*continuing_ == websocket_opcode::text) {
      auto const whole_text = detail::utf8_prefix(std::string_view{data_}.substr(checked_));

      if (!whole_text || (fin && checked_ + *whole_text != data_.size())) {
        fail(error::websocket_invalid_text);
      }

      checked_ += *whole_text;
    }

    if (!fin) {
      return std::nullopt;
    }

    auto const kind = *continuing_;

    continuing_.reset();

    return websocket_message{.opcode = kind, .data = std::exchange(data_, {}), .close_code = 0};
  }

  std::string payload;

  take_into(payload);
  position_ += header + size;

  switch (opcode) {
  case websocket_opcode::close: {
    // Closed only by a close it reads whole: a broken one is a failure, not an end.
    switch (detail::check_close_payload(payload)) {
    case detail::close_payload::broken:
      fail(error::websocket_protocol);
    case detail::close_payload::not_text:
      fail(error::websocket_invalid_text);
    case detail::close_payload::valid:
      break;
    }

    websocket_message message{.opcode = opcode, .data = {}, .close_code = 1005};

    closed_ = true;

    if (!payload.empty()) {
      message.close_code = detail::close_code_in(payload);
      payload.erase(0, 2);
      message.data = std::move(payload);
    }

    return message;
  }
  default:
    return websocket_message{.opcode = opcode, .data = std::move(payload), .close_code = 0};
  }
}

void websocket_parser::fail(std::error_code code) {
  failure_ = code;
  // Nothing is read again: what it holds goes.
  buffer_ = {};
  data_ = {};
  throw std::system_error{code, "quoll::http::websocket_parser"};
}

std::uint16_t close_code_of(std::error_code const &failure) noexcept {
  if (failure == error::websocket_protocol) {
    return 1002;
  }

  if (failure == error::websocket_invalid_text) {
    return 1007;
  }

  if (failure == error::websocket_too_large) {
    return 1009;
  }

  return 1011;
}

} // namespace quoll::http
