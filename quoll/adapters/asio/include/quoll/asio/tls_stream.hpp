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
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::asio {

namespace detail {

// TLS's closing message of a session, as bytes, nothing if it cannot give one (before the end of its handshake). Asio's
// async_shutdown sends it, but goes on to wait for the peer's, and fails on anything else: a peer still sending has
// to be read instead. So OpenSSL writes it to a buffer of ours, for the socket.
[[nodiscard]] inline std::optional<std::string> closing_message(SSL *session) {
  std::unique_ptr<BIO, decltype(&::BIO_free)> const buffer{::BIO_new(::BIO_s_mem()), &::BIO_free};

  if (!buffer) {
    return std::nullopt;
  }

  // Each of these takes over the reference it is given, and releases the one it replaces: `buffer` keeps its own.
  BIO *const original = ::SSL_get_wbio(session);

  ::BIO_up_ref(original);
  ::BIO_up_ref(buffer.get());
  ::SSL_set0_wbio(session, buffer.get());

  auto const shutdown = ::SSL_shutdown(session);

  ::SSL_set0_wbio(session, original);
  ::ERR_clear_error();

  // OpenSSL would now refuse what the peer still sends, and reading it is the point of stopping only this side. Nothing
  // is written after this: the message left is what the flag stands for.
  ::SSL_set_shutdown(session, ::SSL_get_shutdown(session) & ~SSL_SENT_SHUTDOWN);

  if (shutdown < 0) {
    return std::nullopt;
  }

  char *bytes = nullptr;
  auto const size = BIO_get_mem_data(buffer.get(), &bytes);

  return std::string(bytes, static_cast<std::size_t>(size));
}

} // namespace detail

/**
 * @brief ByteStream over a TLS session on a connected asio TCP socket.
 *
 * Behaves as a tcp_stream, once handshake() has completed. The peer closing its side with TLS's closing message is
 * `io::error::end_of_stream`, and without it `std::errc::connection_reset`, as the data may have been cut by anyone. A
 * stop request ends what is pending, and leaves the session unusable but for shutdown_send(): close the stream after
 * that. shutdown_send() sends TLS's closing message, and destroying the stream without it closes without one.
 */
class tls_stream : public detail::basic_stream<::asio::ssl::stream<::asio::ip::tcp::socket>> {
public:
  using session_type = ::asio::ssl::stream<::asio::ip::tcp::socket>;

  /** @brief Takes a session over a connected socket over. */
  explicit tls_stream(session_type session) : basic_stream(std::move(session)) {}

  /** @brief Negotiates the session: as a client, this checks the server. Before any read or write. */
  [[nodiscard]] async::task<std::error_code> handshake(session_type::handshake_type type) {
    return shake_hands(checked_state(), type);
  }

  /**
   * @brief Sends TLS's closing message and stops sending: see io::HalfClosableStream. What the peer sends can still be
   * read. Reports `std::errc::not_connected` for a session that cannot send it, before the end of its handshake. The
   * message goes once: a second call, after one that was stopped or failed, only shuts the socket down, and a write
   * after it fails, as the socket refuses it.
   */
  [[nodiscard]] async::task<io::io_result> shutdown_send() { return shut_down(checked_state()); }

  /** @brief The session, for what OpenSSL offers. */
  [[nodiscard]] session_type &session() noexcept { return checked_state().stream; }

private:
  // On the state, bound at the call: the stream may move before its task runs.
  static async::task<std::error_code> shake_hands(detail::stream_state<session_type> &state,
                                                  session_type::handshake_type type) {
    auto const [error] = co_await detail::make_operation<>(
        state.read, [&state, type](auto handler) { state.stream.async_handshake(type, std::move(handler)); });

    co_return detail::to_io_error(error);
  }

  static async::task<io::io_result> shut_down(detail::stream_state<session_type> &state) {
    std::optional<std::string> message;

    // On the executor of the session, like everything touching it.
    auto const [error, written] =
        co_await detail::make_operation<std::size_t>(state.write, [&state, &message](auto handler) {
          // Once: a second closing message would follow the first, or half of it.
          if (state.closing_sent) {
            std::move(handler)(std::error_code{}, std::size_t{0});
            return;
          }

          message = detail::closing_message(state.stream.native_handle());

          if (message) {
            state.closing_sent = true;
          }

          if (!message) {
            std::move(handler)(std::make_error_code(std::errc::not_connected), std::size_t{0});
            return;
          }

          ::asio::async_write(state.stream.next_layer(), ::asio::buffer(*message), std::move(handler));
        });

    if (error) {
      co_return io::io_result{detail::to_io_error(error), written};
    }

    auto const [closed] = co_await detail::make_operation<>(state.write, [&state](auto handler) {
      std::error_code shutdown;

      state.stream.next_layer().shutdown(::asio::ip::tcp::socket::shutdown_send, shutdown);
      std::move(handler)(shutdown);
    });

    co_return io::io_result{detail::to_io_error(closed), 0};
  }
};

static_assert(async::ContextAwaitable<tls_stream::read_awaitable>);
static_assert(async::ContextAwaitable<tls_stream::write_awaitable>);
static_assert(io::HalfClosableStream<tls_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_TLS_STREAM_HPP
