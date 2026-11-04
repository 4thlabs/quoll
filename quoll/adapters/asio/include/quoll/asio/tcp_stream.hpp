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

#ifndef QUOLL_ASIO_TCP_STREAM_HPP
#define QUOLL_ASIO_TCP_STREAM_HPP

#include <quoll/asio/detail/basic_stream.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/io/byte_stream.hpp>

#include <asio/ip/tcp.hpp>

#include <utility>

namespace quoll::asio {

/**
 * @brief ByteStream over a connected asio TCP socket.
 *
 * Operations complete on the socket's executor, then resume their coroutine on its own executor, which may be
 * another one (a thread pool). A stop request on the awaiter's context, from any thread, cancels the pending
 * operation, which then reports `std::errc::operation_canceled`. The peer closing its side is
 * `io::error::end_of_stream`. Like any asio I/O object, the socket needs an executor that runs one handler at a time:
 * an io_context run by one thread, or a strand.
 *
 * Move-only. The socket lives on the heap: moving the stream is cheap, and allowed while an operation is pending.
 * Destroying it is not, nor destroying its io_context: the waiting coroutine would never resume, and a later stop
 * request would post to a dead executor. Thread safety: none, like the socket.
 */
class tcp_stream : public detail::basic_stream<::asio::ip::tcp::socket> {
public:
  /** @brief Takes a connected socket over. */
  explicit tcp_stream(::asio::ip::tcp::socket socket) : basic_stream(std::move(socket)) {}

  /** @brief The socket, for its options, shutdown() and close(). */
  [[nodiscard]] ::asio::ip::tcp::socket &socket() noexcept { return checked_state().stream; }
};

static_assert(async::ContextAwaitable<tcp_stream::read_awaitable>);
static_assert(async::ContextAwaitable<tcp_stream::write_awaitable>);
static_assert(io::ByteStream<tcp_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TCP_STREAM_HPP
