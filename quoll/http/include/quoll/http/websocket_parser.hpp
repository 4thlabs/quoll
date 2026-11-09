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

#ifndef QUOLL_HTTP_WEBSOCKET_PARSER_HPP
#define QUOLL_HTTP_WEBSOCKET_PARSER_HPP

#include <quoll/http/websocket_message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace quoll::http {

/** @brief Which end of a WebSocket connection reads: a server reads masked frames, a client unmasked ones. */
enum class websocket_role : std::uint8_t { server, client };

/**
 * @brief Reads the messages of a WebSocket connection as its bytes come (RFC 6455): frames, their masks, fragments
 * joined into one message, control frames between them. It does no IO.
 *
 * @code
 * http::websocket_parser messages{http::websocket_role::server};
 *
 * messages.feed(bytes);
 *
 * while (auto message = messages.next()) {
 *   handle(*message);
 * }
 * @endcode
 *
 * A peer that breaks the protocol fails it for good, with an http::error that close_code_of() turns into the code of
 * the close to send: a frame masked or not as its role says it must not be, a reserved bit or opcode, a length not in
 * its shortest form, a control frame fragmented or longer than 125 bytes, a continuation with no message to continue
 * or a message started inside another, a close with a code no one may send (`websocket_protocol`, 1002); text, or a
 * close's reason, that is not UTF-8 (`websocket_invalid_text`, 1007); a message larger than the
 * most it holds (`websocket_too_large`, 1009). After a close, it reads nothing more.
 *
 * Thread safety: none, one user at a time.
 */
class websocket_parser {
public:
  /** @brief The most a message holds by default, its fragments joined: 1 MiB. */
  static constexpr std::size_t default_max_message_size = std::size_t{1} << 20U;

  /** @brief A parser for `role`'s end, holding at most `max_message_size` bytes of a message. */
  explicit websocket_parser(websocket_role role, std::size_t max_message_size = default_max_message_size) noexcept
      : role_(role), max_message_size_(max_message_size) {}

  /**
   * @brief Adds bytes of the connection, which next() reads. They are copied. Call next() until it gives nothing before
   * the next piece: the parser then holds no more than a piece, the frame being read and the message it joins, about
   * twice the most a message holds at worst.
   *
   * @throws std::system_error with what next() threw once it has, and std::logic_error while next() has not given
   * every message of the bytes fed before.
   */
  void feed(std::string_view bytes);

  /**
   * @brief The next message the bytes fed so far end, or none until more comes, or after a close.
   *
   * @throws std::system_error with an http::error for a peer that breaks the protocol (see the class), and on every
   * call after.
   */
  [[nodiscard]] std::optional<websocket_message> next();

  /** @brief Whether a close has been read: nothing follows it. */
  [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
  // The frame at position_, if whole: its message, if it ends one.
  [[nodiscard]] std::optional<websocket_message> frame(bool &whole);

  [[noreturn]] void fail(std::error_code code);

  websocket_role role_;
  std::size_t max_message_size_;
  // What was fed, from position_ on not read yet.
  std::string buffer_;
  std::size_t position_ = 0;
  // The message being joined from its fragments, if one is.
  std::optional<websocket_opcode> continuing_;
  std::string data_;
  // How much of a text message being joined is checked as UTF-8.
  std::size_t checked_ = 0;
  bool closed_ = false;
  bool drained_ = true;
  std::error_code failure_;
};

/** @brief The code of the close to send for what websocket_parser threw: 1002, 1007 or 1009; 1011 for anything else. */
[[nodiscard]] std::uint16_t close_code_of(std::error_code const &failure) noexcept;

} // namespace quoll::http

#endif // QUOLL_HTTP_WEBSOCKET_PARSER_HPP
