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

#ifndef QUOLL_ASIO_UNIX_STREAM_HPP
#define QUOLL_ASIO_UNIX_STREAM_HPP

#include <asio/detail/config.hpp>

#if defined(ASIO_HAS_LOCAL_SOCKETS)

#include <quoll/asio/detail/basic_stream.hpp>
#include <quoll/asio/detail/poll_socket.hpp>
#include <quoll/asio/detail/shut_down.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <asio/local/stream_protocol.hpp>

#include <utility>

namespace quoll::asio {

/**
 * @brief ByteStream over a connected asio Unix domain socket, as tcp_stream is over TCP: the same operations, stops and
 * errors, the same executor a socket needs, one handler at a time.
 *
 * Move-only, the socket on the heap. Thread safety: none, like the socket. Where the system has such sockets
 * (ASIO_HAS_LOCAL_SOCKETS).
 */
class unix_stream : public detail::basic_stream<::asio::local::stream_protocol::socket> {
public:
  /** @brief Takes a connected socket over. */
  explicit unix_stream(::asio::local::stream_protocol::socket socket) : basic_stream(std::move(socket)) {}

  /** @brief Stops sending: see io::HalfClosableStream. */
  [[nodiscard]] async::task<io::io_result> shutdown_send() { return detail::shut_down(checked_state()); }

  /** @brief What a read started now would do, as a poll of the socket that does not wait tells: see
   * io::PollableStream. */
  [[nodiscard]] io::read_readiness poll_read() noexcept { return detail::poll_socket(checked_state().stream); }

  /** @brief The socket, for its options, shutdown() and close(). */
  [[nodiscard]] ::asio::local::stream_protocol::socket &socket() noexcept { return checked_state().stream; }
};

static_assert(async::ContextAwaitable<unix_stream::read_awaitable>);
static_assert(async::ContextAwaitable<unix_stream::write_awaitable>);
static_assert(io::HalfClosableStream<unix_stream>);
static_assert(io::PollableStream<unix_stream>);

} // namespace quoll::asio

#endif // ASIO_HAS_LOCAL_SOCKETS

#endif // QUOLL_ASIO_UNIX_STREAM_HPP
