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

#ifndef QUOLL_ASIO_TLS_STREAM_HPP
#define QUOLL_ASIO_TLS_STREAM_HPP

// Only with OpenSSL: see QUOLL_TLS in this adapter's CMakeLists.txt.
#if defined(QUOLL_ASIO_TLS)

#include <quoll/asio/detail/basic_stream.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/byte_stream.hpp>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include <system_error>
#include <utility>

namespace quoll::asio {

/**
 * @brief ByteStream over a TLS session on a connected asio TCP socket.
 *
 * Behaves as a tcp_stream, once handshake() has completed. The peer closing its side with TLS's closing message is
 * `io::error::end_of_stream`, and without it `std::errc::connection_reset`, as the data may have been cut by anyone. A
 * stop request ends what is pending, and leaves the session unusable: close the stream. Destroying it closes the socket
 * without sending TLS's closing message.
 */
class tls_stream : public detail::basic_stream<::asio::ssl::stream<::asio::ip::tcp::socket>> {
public:
  using session_type = ::asio::ssl::stream<::asio::ip::tcp::socket>;

  /** @brief Takes a session over a connected socket over. */
  explicit tls_stream(session_type session) : basic_stream(std::move(session)) {}

  /** @brief Negotiates the session: as a client, this checks the server. Before any read or write. */
  [[nodiscard]] async::task<std::error_code> handshake(session_type::handshake_type type) {
    auto &state = checked_state();

    auto const [error] = co_await detail::make_operation<>(
        state.read, [&state, type](auto handler) { state.stream.async_handshake(type, std::move(handler)); });

    co_return detail::to_io_error(error);
  }

  /** @brief The session, for what OpenSSL offers. */
  [[nodiscard]] session_type &session() noexcept { return checked_state().stream; }
};

static_assert(async::ContextAwaitable<tls_stream::read_awaitable>);
static_assert(async::ContextAwaitable<tls_stream::write_awaitable>);
static_assert(io::ByteStream<tls_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_TLS_STREAM_HPP
