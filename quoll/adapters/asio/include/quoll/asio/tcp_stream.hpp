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
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/asio/detail/poll_socket.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <asio/ip/tcp.hpp>

#include <system_error>
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

  /** @brief Stops sending: see io::HalfClosableStream. A write after it fails, with `std::errc::broken_pipe` from the
   * socket. */
  [[nodiscard]] async::task<io::io_result> shutdown_send() { return shut_down(checked_state()); }

  /** @brief What a read started now would do, as a poll of the socket that does not wait tells: see
   * io::PollableStream. */
  [[nodiscard]] io::read_readiness poll_read() noexcept { return detail::poll_socket(checked_state().stream); }

  /** @brief The socket, for its options, shutdown() and close(). */
  [[nodiscard]] ::asio::ip::tcp::socket &socket() noexcept { return checked_state().stream; }

private:
  // On the state, bound at the call: the stream may move before its task runs.
  static async::task<io::io_result> shut_down(detail::stream_state<::asio::ip::tcp::socket> &state) {
    auto const [error] = co_await detail::make_operation<>(state.write, [&state](auto handler) {
      std::error_code shutdown;

      if (!state.sending_shut) {
        state.stream.shutdown(::asio::ip::tcp::socket::shutdown_send, shutdown);
        state.sending_shut = !shutdown;
      }

      std::move(handler)(shutdown);
    });

    co_return io::io_result{detail::to_io_error(error), 0};
  }
};

static_assert(async::ContextAwaitable<tcp_stream::read_awaitable>);
static_assert(async::ContextAwaitable<tcp_stream::write_awaitable>);
static_assert(io::HalfClosableStream<tcp_stream>);
static_assert(io::PollableStream<tcp_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TCP_STREAM_HPP
