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

#ifndef QUOLL_HTTP_READ_HPP
#define QUOLL_HTTP_READ_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>

namespace quoll::http {

namespace detail {

// Reads until `reached()`. Awaited by what follows, whose documentation says how it ends.
template <io::ByteStream Stream, class Message, std::predicate Reached>
[[nodiscard]] async::task<std::error_code> read_until(Stream &stream, std::string &buffer,
                                                      parser<Message> &message_parser, Reached reached) {
  // In the coroutine frame rather than in `buffer`: an exception or a destroyed task leaves `buffer` untouched.
  std::array<char, 4096> chunk;

  for (;;) {
    if (!buffer.empty()) {
      auto const parsed = message_parser.feed(buffer);

      buffer.erase(0, parsed.consumed);

      // Reached first: how the bytes were cut must not change what comes of them, as what was reached is good, and a
      // failure past it is for what reads on.
      if (reached()) {
        co_return std::error_code{};
      }

      if (parsed.error) {
        co_return parsed.error;
      }
    } else if (auto const failure = message_parser.failure()) {
      // It stopped with no bytes left to feed: nothing that comes can change that.
      co_return failure;
    }

    auto const received = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    assert((!received.error || received.transferred == 0) && "ByteStream::read_some() returned bytes with an error");
    assert((received.error || received.transferred > 0) && "ByteStream::read_some() read nothing without an error");
    buffer.append(chunk.data(), received.transferred);

    if (received.error == io::error::end_of_stream) {
      if (auto const finished = message_parser.finish()) {
        co_return finished;
      }

      co_return message_parser.done() ? std::error_code{} : std::error_code{io::error::end_of_stream};
    }

    if (received.error) {
      co_return received.error;
    }
  }
}

} // namespace detail

/**
 * @brief Reads one message from `stream` into `message_parser`; release() it from there once read.
 *
 * `buffer` holds the bytes read but not parsed yet, and belongs to the connection: after a message it may hold the
 * start of the next one (pipelining), or the first bytes of an upgraded protocol.
 *
 * Completes with no error once the parser is done(), including for a body delimited by the end of the stream, or at the
 * end of the head of a message whose body comes in pieces, for read_body();
 * `io::error::end_of_stream` when the peer closed the stream before a new message started; otherwise with the
 * parser's error (`http::error`) or the stream's. `stream`, `buffer` and `message_parser` must outlive the task.
 * Precondition: the parser holds no complete message.
 */
template <io::ByteStream Stream, class Message>
[[nodiscard]] async::task<std::error_code> read(Stream &stream QUOLL_LIFETIMEBOUND,
                                                std::string &buffer QUOLL_LIFETIMEBOUND,
                                                parser<Message> &message_parser QUOLL_LIFETIMEBOUND) {
  assert(!message_parser.done() && "http::read() with a message not released yet");

  // Going on would hold in memory what the larger limit of a body in pieces lets through.
  return detail::read_until(stream, buffer, message_parser, [&message_parser] {
    return message_parser.done() || (message_parser.head_complete() && message_parser.body_streamed());
  });
}

/**
 * @brief Like read(), up to the end of the message's head: parser::head_complete(), and perhaps done() if the rest came
 * with it. read() or read_body() then goes on with the body.
 */
template <io::ByteStream Stream, class Message>
[[nodiscard]] async::task<std::error_code> read_head(Stream &stream QUOLL_LIFETIMEBOUND,
                                                     std::string &buffer QUOLL_LIFETIMEBOUND,
                                                     parser<Message> &message_parser QUOLL_LIFETIMEBOUND) {
  assert(!message_parser.done() && "http::read_head() with a message not released yet");

  return detail::read_until(stream, buffer, message_parser,
                            [&message_parser] { return message_parser.head_complete(); });
}

/**
 * @brief Reads the next piece of a body in pieces (see parser::set_body_streaming()) into `piece`, replacing what it
 * held.
 *
 * Completes with no error once there is a piece, or once the parser is done(), with what is left of the body, perhaps
 * nothing: release() then hands over the message, with its trailers and no body. Fails like read(), once what came
 * before the failure has been handed over. Memory holds what one read of the stream brings, whatever the size of the
 * body. `stream`, `buffer`, `message_parser` and `piece` must outlive the task. Precondition: parser::head_complete(),
 * as read_head() gets it, and parser::body_streamed().
 */
template <io::ByteStream Stream, class Message>
[[nodiscard]] async::task<std::error_code>
read_body(Stream &stream QUOLL_LIFETIMEBOUND, std::string &buffer QUOLL_LIFETIMEBOUND,
          parser<Message> &message_parser QUOLL_LIFETIMEBOUND, std::string &piece QUOLL_LIFETIMEBOUND) {
  assert(message_parser.head_complete() && message_parser.body_streamed() &&
         "http::read_body() for a message whose body does not come in pieces");

  auto const has_news = [&message_parser] { return message_parser.done() || !message_parser.head().body.empty(); };
  std::error_code error;

  if (!has_news()) {
    error = co_await detail::read_until(stream, buffer, message_parser, has_news);
  }

  message_parser.take_body(piece);
  co_return error;
}

} // namespace quoll::http

#endif // QUOLL_HTTP_READ_HPP
