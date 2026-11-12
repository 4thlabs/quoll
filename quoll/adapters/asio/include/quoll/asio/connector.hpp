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

#ifndef QUOLL_ASIO_CONNECTOR_HPP
#define QUOLL_ASIO_CONNECTOR_HPP

#include <quoll/asio/detail/connect.hpp>
#include <quoll/asio/detail/tls_connect.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>

#include <asio/any_io_executor.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace quoll::asio {

/**
 * @brief Connector opening TCP connections with asio: resolves the host, then tries its addresses in turn.
 *
 * The connection is a tcp_stream on `executor`, handed over as an io::any_stream. Resolution and connection failures
 * throw std::system_error from the task, with asio's error code (`asio::error::connection_refused`...). A stop
 * request, from any thread, abandons the attempt, which ends the awaiter on the stop. `executor` runs one handler
 * at a time (an io_context run by one thread, or a strand).
 *
 * The task copies what it needs: the connector does not have to outlive it.
 *
 * Thread safety: connect() may be called from any thread, concurrently; each attempt has its own resolver and socket.
 */
class connector {
public:
  explicit connector(::asio::any_io_executor executor) noexcept : executor_(std::move(executor)) {}

  /** @brief Connects to `host` (a name or an address) on `port`. */
  [[nodiscard]] async::task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return detail::connect(executor_, std::move(host), port);
  }

#if defined(QUOLL_ASIO_TLS)
  /** @brief Connects to `host` (a name or an address) on `port` over TLS, checking it as `options` says. */
  [[nodiscard]] async::task<io::any_stream>
  connect_tls(std::string host, std::uint16_t port, io::tls_client_options const &options) const {
    return detail::connect_tls(executor_, std::move(host), port, options);
  }
#endif

private:
  ::asio::any_io_executor executor_;
};

static_assert(io::Connector<connector>);
#if defined(QUOLL_ASIO_TLS)
static_assert(io::TlsConnector<connector>);
#endif

} // namespace quoll::asio

#endif // QUOLL_ASIO_CONNECTOR_HPP
