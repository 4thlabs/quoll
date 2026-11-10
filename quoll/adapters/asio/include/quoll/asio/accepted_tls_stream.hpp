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

#ifndef QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP
#define QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP

// Only with OpenSSL: see QUOLL_TLS in this adapter's CMakeLists.txt.
#if defined(QUOLL_ASIO_TLS)

#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <asio/ssl/context.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace quoll::asio {

/**
 * @brief A TLS session a tls_acceptor accepted, as its accept() gives it in an io::any_stream: recover it with
 * `target<asio::accepted_tls_stream>()`, and what OpenSSL negotiated with session().
 *
 * Its handshake, as a server, takes place on its first read, in the connection's own task rather than in the accept
 * loop, where a slow peer would hold every other one back; the timeouts of a read apply to it too. A server that speaks
 * first calls handshake() before writing: a write before the handshake has completed throws std::logic_error. A
 * handshake that failed or was stopped leaves the session unusable: every later operation reports its error.
 *
 * Move-only, as its tls_stream. Thread safety: none, as its tls_stream.
 */
class accepted_tls_stream {
public:
  /** @brief Takes over a session on an accepted socket, made from `context`, which it keeps for as long as it lives. */
  accepted_tls_stream(tls_stream stream, std::shared_ptr<::asio::ssl::context> context) noexcept
      : context_(std::move(context)), stream_(std::move(stream)) {}

  /**
   * @brief Negotiates the session as a server, once: the first read does it if this has not. Reports the error of a
   * handshake that failed or was stopped, again on every later call.
   */
  [[nodiscard]] async::task<std::error_code> handshake() {
    if (!handshaken_ && !failure_) {
      failure_ = co_await stream_.handshake(tls_stream::session_type::server);
      handshaken_ = !failure_;
    }

    co_return failure_;
  }

  /** @brief Reads what the peer sent, after the handshake for the first read; a failed handshake is its error. */
  [[nodiscard]] async::task<io::io_result> read_some(std::span<std::byte> buffer) {
    if (auto const error = co_await handshake()) {
      co_return io::io_result{error, 0};
    }

    co_return co_await stream_.read_some(buffer);
  }

  /**
   * @brief Sends some of `data`, once the handshake has completed.
   * @throws std::logic_error before it has; std::system_error with its error once it has failed.
   */
  [[nodiscard]] tls_stream::write_awaitable write_some(std::span<std::byte const> data) {
    if (failure_) {
      throw std::system_error{failure_, "quoll::asio::accepted_tls_stream: its handshake failed"};
    }

    if (!handshaken_) {
      throw std::logic_error{"quoll::asio::accepted_tls_stream: written to before its handshake"};
    }

    return stream_.write_some(data);
  }

  /** @brief Sends TLS's closing message and stops sending: see tls_stream::shutdown_send(). */
  [[nodiscard]] async::task<io::io_result> shutdown_send() { return stream_.shutdown_send(); }

  /** @brief What a read started now would do, once handshaken: see tls_stream::poll_read(). Unknown before. */
  [[nodiscard]] io::read_readiness poll_read() noexcept {
    return handshaken_ ? stream_.poll_read() : io::read_readiness::unknown;
  }

  /** @brief Whether the handshake has completed. */
  [[nodiscard]] bool handshaken() const noexcept { return handshaken_; }

  /**
   * @brief The session, for what OpenSSL negotiated once handshaken(): its peer's certificate, its protocol. Reading
   * or writing through it goes around this stream.
   */
  [[nodiscard]] tls_stream::session_type &session() noexcept { return stream_.session(); }

  /** @copydoc session */
  [[nodiscard]] tls_stream::session_type const &session() const noexcept { return stream_.session(); }

private:
  // Before the session, which is made from it.
  std::shared_ptr<::asio::ssl::context> context_;
  tls_stream stream_;
  std::error_code failure_;
  bool handshaken_ = false;
};

static_assert(io::HalfClosableStream<accepted_tls_stream>);
static_assert(io::PollableStream<accepted_tls_stream>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_ACCEPTED_TLS_STREAM_HPP
