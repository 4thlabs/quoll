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

#include <quoll/async/task.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>

#include <cassert>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>

namespace quoll::http {

/**
 * @brief Reads one message from `stream` into `message_parser`; release() it from there once read.
 *
 * `buffer` holds the bytes read but not parsed yet, and belongs to the connection: after a message it may hold the
 * start of the next one (pipelining), or the first bytes of an upgraded protocol.
 *
 * Completes with no error once the parser is done(), including for a body delimited by the end of the stream; with
 * `io::error::end_of_stream` when the peer closed the stream before a new message started; otherwise with the
 * parser's error (`http::error`) or the stream's. `stream`, `buffer` and `message_parser` must outlive the task.
 * Precondition: the parser holds no complete message.
 */
template <io::ByteStream Stream, class Message>
[[nodiscard]] async::task<std::error_code> read(Stream &stream, std::string &buffer, parser<Message> &message_parser) {
  assert(!message_parser.done() && "http::read() with a message not released yet");

  constexpr std::size_t chunk_size = 4096;

  for (;;) {
    if (!buffer.empty()) {
      auto const parsed = message_parser.feed(buffer);

      buffer.erase(0, parsed.consumed);

      if (parsed.error) {
        co_return parsed.error;
      }

      if (message_parser.done()) {
        co_return std::error_code{};
      }
    }

    // The parser took every byte: the buffer is empty again, and only grows by what one read brings.
    auto const size = buffer.size();

    buffer.resize(size + chunk_size);

    auto const received = co_await stream.read_some(std::as_writable_bytes(std::span{buffer}.subspan(size)));

    buffer.resize(size + received.transferred);

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

} // namespace quoll::http

#endif // QUOLL_HTTP_READ_HPP
