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

#ifndef QUOLL_HTTP_PARSER_HPP
#define QUOLL_HTTP_PARSER_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/verb.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace quoll::http {

/** @brief Size limits of a parser, against peers sending unbounded messages. */
struct parse_limits {
  /** @brief Start line, fields and trailers together, separators included. */
  std::size_t max_header_size = 64 * 1024;
  /** @brief Fields and trailers together: bounds the memory of many tiny fields. */
  std::size_t max_field_count = 100;
  std::size_t max_body_size = 8 * 1024 * 1024;
  /** @brief A body handed over in pieces (see parser::set_body_streaming()), which memory does not have to hold. */
  std::uint64_t max_streamed_body_size = std::uint64_t{1024} * 1024 * 1024;
};

/** @brief Outcome of parser::feed(). */
struct parse_result {
  /** @brief Bytes used from the input; the rest belongs to the next message (or to the upgraded protocol). */
  std::size_t consumed = 0;
  std::error_code error;
};

/**
 * @brief Incremental, sans-IO HTTP/1.x parser (llhttp) for a request or a response.
 *
 * Feed it bytes as they arrive: feed() stops at the end of a message and reports what it consumed, so the bytes of
 * a pipelined next message stay with the caller. Once done(), release() hands the message over and the parser is
 * ready for the next one on the same connection. When the peer closes the connection, call finish(): it completes
 * a body delimited by the end of the stream, or reports a truncated message.
 *
 * Responses: interim 1xx responses are messages of their own (release and keep feeding for the final one), and a
 * response to HEAD needs set_request_method(verb::head) first, as its Content-Length does not announce a body.
 *
 * Holds its state apart, so that a program does not see the parser behind it. Moving one keeps its progress; a
 * moved-from one can only be assigned to or destroyed.
 *
 * Thread safety: none.
 */
template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
class parser {
  static constexpr bool is_request = std::same_as<Message, request>;

public:
  using message_type = Message;

  explicit parser(parse_limits limits = {});

  parser(parser const &) = delete;
  parser &operator=(parser const &) = delete;
  parser(parser &&other) noexcept;
  parser &operator=(parser &&other) noexcept;
  ~parser();

  /** @brief Parses `data` up to the end of the current message. Does nothing once done() or after an error. */
  [[nodiscard]] parse_result feed(std::span<char const> data);

  /**
   * @brief The peer closed the stream: ends a body delimited by it, or reports a truncated message. Without an
   * error, the parser is then ready for a new stream.
   */
  [[nodiscard]] std::error_code finish();

  /**
   * @brief Responses only: the method of the request answered, so that a response to HEAD has no body and a 2xx
   * response to CONNECT switches protocol.
   */
  void set_request_method(verb method) noexcept
    requires(!is_request);

  /**
   * @brief Asks `decide`, once the start line and fields of each message are parsed, whether its body comes in pieces:
   * every message, those without a body too, as interim responses are: it says, and does nothing.
   *
   * If it does, parse_limits::max_streamed_body_size bounds it instead of max_body_size, and take_body() hands over
   * what has come: what is not taken stays in head(), so that memory holds as much as is fed between two calls.
   * Otherwise the message is read whole, as if nothing had been asked. `decide` must not throw, like what the parser
   * allocates.
   */
  void set_body_streaming(std::function<bool(Message const &head)> decide);

  /** @brief The body of the current message comes in pieces. Known once head_complete(). Reset by release(). */
  [[nodiscard]] bool body_streamed() const noexcept;

  /**
   * @brief Puts in `piece` what has come of a body in pieces since the last call, and nothing if nothing has, or if the
   * body is read whole.
   */
  void take_body(std::string &piece) noexcept;

  /** @brief A complete message is waiting for release(). */
  [[nodiscard]] bool done() const noexcept;

  /** @brief The start line and fields of the current message are parsed: head() holds them. Reset by release(). */
  [[nodiscard]] bool head_complete() const noexcept;

  /** @brief The message so far: its start line and fields, once head_complete(), and what has come of its body. */
  [[nodiscard]] Message const &head() const noexcept QUOLL_LIFETIMEBOUND;

  /** @brief When done(): whether the connection may carry another message. */
  [[nodiscard]] bool keep_alive() const noexcept;

  /**
   * @brief When done(): the connection switches protocol (101, CONNECT); its next bytes are not HTTP/1.x, and
   * feeding them after release() reports `error::upgraded`.
   */
  [[nodiscard]] bool upgrade() const noexcept;

  /** @brief Why the input was rejected, for diagnostics. */
  [[nodiscard]] std::string_view error_reason() const noexcept;

  /**
   * @brief Takes the parsed message and gets ready for the next one. Precondition: done().
   *
   * keep_alive() and upgrade() are reset; for responses, the request method set by set_request_method() is
   * forgotten after a final (non-1xx) response.
   */
  [[nodiscard]] Message release();

private:
  struct impl;

  std::unique_ptr<impl> impl_;
};

extern template class parser<request>;
extern template class parser<response>;

using request_parser = parser<request>;
using response_parser = parser<response>;

} // namespace quoll::http

#endif // QUOLL_HTTP_PARSER_HPP
