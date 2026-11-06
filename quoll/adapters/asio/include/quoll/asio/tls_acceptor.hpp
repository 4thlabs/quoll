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

#include <quoll/asio/detail/accept.hpp>
#include <quoll/asio/detail/tls_connect.hpp>
#include <quoll/asio/tls_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/bound_listener.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/tls_options.hpp>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

// A session accepted by a tls_acceptor: its handshake takes place on its first read, in the connection's own task. In
// the accept loop, a slow peer would hold every other one back; here, the timeouts of a read apply to it too.
class accepted_tls_stream {
public:
  accepted_tls_stream(tls_stream stream, std::shared_ptr<::asio::ssl::context> context) noexcept
      : stream_(std::move(stream)), context_(std::move(context)) {}

  [[nodiscard]] async::task<io::io_result> read_some(std::span<std::byte> buffer) {
    if (!shaken_hands_) {
      if (auto const error = co_await stream_.handshake(tls_stream::session_type::server)) {
        co_return io::io_result{error, 0};
      }

      shaken_hands_ = true;
    }

    co_return co_await stream_.read_some(buffer);
  }

  // A server answers what it has read: the handshake has taken place.
  [[nodiscard]] auto write_some(std::span<std::byte const> data) {
    assert(shaken_hands_ && "accepted_tls_stream written to before its first read");
    return stream_.write_some(data);
  }

  [[nodiscard]] auto shutdown_send() { return stream_.shutdown_send(); }

private:
  tls_stream stream_;
  std::shared_ptr<::asio::ssl::context> context_;
  bool shaken_hands_ = false;
};

static_assert(io::HalfClosableStream<accepted_tls_stream>);

} // namespace detail

/**
 * @brief Listener accepting TLS connections with asio, for a server (quoll::http::server): as an acceptor, with a
 * certificate. Each connection's handshake takes place on its first read.
 *
 * Move-only; the acceptor lives on the heap, so moving it is allowed while an accept is pending. Destroying it, or its
 * io_context, is not. One accept at a time. Thread safety: none, like the acceptor.
 */
class tls_acceptor {
public:
  /**
   * @brief Takes a listening acceptor over, and loads the certificate of `options`.
   *
   * @throws std::system_error when it cannot be loaded, or the key is not the certificate's.
   */
  tls_acceptor(::asio::ip::tcp::acceptor listening, io::tls_server_options const &options)
      : state_(std::make_unique<detail::acceptor_state>(std::move(listening))),
        context_(detail::server_context(options)) {}

  /** @brief The next connection a peer opens. */
  [[nodiscard]] async::task<io::any_stream> accept() {
    assert(state_ && "tls_acceptor used after being moved from");

    co_return io::any_stream{detail::accepted_tls_stream{
        tls_stream{tls_stream::session_type{co_await detail::accept_socket(*state_), *context_}}, context_}};
  }

  /** @brief The port it accepts on: the one the system picked when it was bound to port 0. */
  [[nodiscard]] std::uint16_t local_port() const {
    assert(state_ && "tls_acceptor used after being moved from");
    return state_->acceptor.local_endpoint().port();
  }

private:
  std::unique_ptr<detail::acceptor_state> state_;
  std::shared_ptr<::asio::ssl::context> context_;
};

static_assert(io::BoundListener<tls_acceptor>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_TLS

#endif // QUOLL_ASIO_TLS_ACCEPTOR_HPP
