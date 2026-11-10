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

#ifndef QUOLL_ASIO_TLS_ACCEPTOR_HPP
#define QUOLL_ASIO_TLS_ACCEPTOR_HPP

// Only with OpenSSL: see QUOLL_TLS in this adapter's CMakeLists.txt.
#if defined(QUOLL_ASIO_TLS)

#include <quoll/asio/accepted_tls_stream.hpp>
#include <quoll/asio/detail/accept.hpp>
#include <quoll/asio/detail/tls_connect.hpp>
#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/bound_listener.hpp>
#include <quoll/io/tls_options.hpp>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace quoll::asio {

namespace detail {

[[nodiscard]] inline std::shared_ptr<::asio::ssl::context> server_context(io::tls_server_options const &options) {
  auto context = std::make_shared<::asio::ssl::context>(::asio::ssl::context::tls_server);

  require_modern_tls(*context);
  context->use_certificate_chain_file(options.certificate_file);
  context->use_private_key_file(options.private_key_file, ::asio::ssl::context::pem);

  return context;
}

// On the state and a context of its own, bound at the call: the acceptor may move while an accept is pending.
inline async::task<io::any_stream> accept_tls(acceptor_state &state, std::shared_ptr<::asio::ssl::context> context) {
  auto socket = co_await accept_socket(state);

  co_return io::any_stream{
      asio::accepted_tls_stream{tls_stream{tls_stream::session_type{std::move(socket), *context}}, std::move(context)}};
}

} // namespace detail

/**
 * @brief Listener accepting TLS connections with asio, for a server (quoll::http::server): as an acceptor, with a
 * certificate. Each connection's handshake takes place on its first read: accept() gives an io::any_stream over an
 * accepted_tls_stream, which `target<asio::accepted_tls_stream>()` recovers, and its stream() the session.
 *
 * Move-only; the acceptor lives on the heap, so moving it is allowed while an accept is pending. Destroying it, or its
 * io_context, is not. One accept at a time. Thread safety: none, like the acceptor.
 */
class tls_acceptor {
public:
  /**
   * @brief Takes a listening acceptor over, and loads the certificate of `options`.
   *
   * Each accepted socket goes on a strand of its own, made from `socket_strands` if any (see acceptor).
   *
   * @throws std::system_error when it cannot be loaded, or the key is not the certificate's.
   */
  tls_acceptor(::asio::ip::tcp::acceptor listening, io::tls_server_options const &options,
               std::optional<::asio::any_io_executor> socket_strands = std::nullopt)
      : state_(std::make_unique<detail::acceptor_state>(std::move(listening), std::move(socket_strands))),
        context_(detail::server_context(options)) {}

  /** @brief The next connection a peer opens, an accepted_tls_stream. */
  [[nodiscard]] async::task<io::any_stream> accept() { return detail::accept_tls(checked_state(), context_); }

  /** @brief The port it accepts on: the one the system picked when it was bound to port 0. */
  [[nodiscard]] std::uint16_t local_port() const { return checked_state().acceptor.local_endpoint().port(); }

private:
  [[nodiscard]] detail::acceptor_state &checked_state() noexcept {
    assert(state_ && "tls_acceptor used after being moved from");
    return *state_;
  }

  [[nodiscard]] detail::acceptor_state const &checked_state() const noexcept {
    assert(state_ && "tls_acceptor used after being moved from");
    return *state_;
  }

  std::unique_ptr<detail::acceptor_state> state_;
  std::shared_ptr<::asio::ssl::context> context_;
};

static_assert(io::BoundListener<tls_acceptor>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_TLS_ACCEPTOR_HPP
