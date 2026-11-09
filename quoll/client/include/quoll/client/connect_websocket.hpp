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

#ifndef QUOLL_CLIENT_CONNECT_WEBSOCKET_HPP
#define QUOLL_CLIENT_CONNECT_WEBSOCKET_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/connected_websocket.hpp>
#include <quoll/client/detail/client_backend.hpp>
#include <quoll/client/detail/connect_websocket.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/client/websocket_connect_options.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/tls_connector.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::http {

/**
 * @brief Opens a WebSocket to `url`, `ws[s]://host[:port][/path][?query]`, through `connector`, on a connection of its
 * own: a client's end, which masks what it sends.
 *
 * @code
 * auto [socket, handshake] = co_await http::connect_websocket(connector, "wss://chat.example.com/room?id=4");
 *
 * co_await socket.send_text("hello");
 *
 * auto const answer = co_await socket.receive();
 * @endcode
 *
 * The server's 101 is checked: its `Sec-WebSocket-Accept`, no extension, no subprotocol but one the fields offered;
 * interim answers before it (103) are skipped. Anything else throws http::websocket_refused, with the server's answer,
 * even one that came as the handshake could not be sent whole. The connection and the handshake take no
 * longer than `options.handshake_timeout`, past which std::errc::timed_out is thrown; a network failure throws the
 * stream's error. Without a timer in the executor, nothing limits them. A client sends no ping on its own: one that
 * wants to find a server gone pings it from a task beside what receives.
 *
 * @throws std::invalid_argument, when called, for another URL, a field of the handshake's own, a subprotocol offered
 * that is no token, a request that would not serialize, or `wss` with a connector that is no io::TlsConnector; and
 * what std::random_device throws, when called, if the key cannot be drawn.
 */
template <io::Connector Connector>
[[nodiscard]] async::task<connected_websocket> connect_websocket(Connector connector, std::string_view url,
                                                                 websocket_connect_options options = {}) {
  auto const where = detail::parse_websocket_url(url);

  if (where.server.secure && !io::TlsConnector<Connector>) {
    throw std::invalid_argument{"quoll::http::connect_websocket: this connector cannot open wss connections"};
  }

  // Before any connection: what would not serialize is refused here.
  auto handshake = detail::prepare_websocket_handshake(where, std::move(options.fields));

  return detail::connect_websocket_through(detail::make_connector_handle(std::move(connector)), where.server,
                                           std::move(handshake), std::move(options));
}

/**
 * @brief connect_websocket() through the network of the executor that connects (io::network_connector), as
 * quoll::runtime provides it natively. In a browser, which offers no network, it throws std::logic_error as it
 * connects: a browser's WebSocket is not there yet.
 */
[[nodiscard]] async::task<connected_websocket> connect_websocket(std::string_view url,
                                                                 websocket_connect_options options = {});

} // namespace quoll::http

#endif // QUOLL_CLIENT_CONNECT_WEBSOCKET_HPP
