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

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client/detail/client_backend.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http {

/** @brief How http::connect_websocket opens a WebSocket. */
struct websocket_connect_options {
  /**
   * @brief Fields the handshake carries besides its own: `Origin`, `Sec-WebSocket-Protocol` with the subprotocols
   * offered, `Authorization`... Those of the handshake itself (`Host`, `Upgrade`, `Connection`, `Sec-WebSocket-Key`,
   * `Sec-WebSocket-Version`, `Sec-WebSocket-Extensions`, `Sec-WebSocket-Accept`) are refused, and those that would give
   * it a body (`Content-Length`, `Transfer-Encoding`).
   */
  http::fields fields{};
  /** @brief What the connection accepts, and how long a write may wait with nothing taken. */
  websocket_options websocket{};
  /** @brief How long connecting and the handshake may take, both, given a timer; zero: no limit. */
  std::chrono::nanoseconds handshake_timeout = std::chrono::seconds{30};
  /** @brief Size limits of the server's answer to the handshake. */
  http::parse_limits limits{};
  /** @brief How `wss` checks the server: by default, against the system's authorities. */
  io::tls_client_options tls{};
};

/** @brief What http::connect_websocket gives: the connection, and the server's 101. */
struct connected_websocket {
  /** @brief The connection, a client's end. */
  websocket socket;
  /** @brief The server's 101, with what it chose in its fields: `Sec-WebSocket-Protocol`, cookies... */
  response handshake;
};

/**
 * @brief Thrown by http::connect_websocket for a server that does not switch to a WebSocket: any other answer, or a 101
 * that is not what the handshake asked for. Its response says which.
 */
class websocket_refused : public std::runtime_error {
public:
  explicit websocket_refused(http::response answer);

  // Copies and no moves: an exception does not throw on its way to a handler, and one moved from stays whole.
  websocket_refused(websocket_refused const &) noexcept = default;
  websocket_refused &operator=(websocket_refused const &) noexcept = default;
  ~websocket_refused() override = default;

  /** @brief The server's answer: its head, the body of a refusal left unread. */
  [[nodiscard]] http::response const &response() const noexcept QUOLL_LIFETIMEBOUND { return *response_; }

private:
  std::shared_ptr<http::response const> response_;
};

namespace detail {

// A client's handshake, ready to send: its request on the wire, its key, and the subprotocols it offers.
struct websocket_handshake {
  std::string wire;
  std::string key;
  std::vector<std::string> offered;
};

// The handshake to `where` with `fields`, checked: std::invalid_argument for a field of the handshake's own, or a
// request that would not serialize.
[[nodiscard]] websocket_handshake prepare_websocket_handshake(websocket_target const &where, http::fields fields);

// The rest of connect_websocket(), through `connector` to `server`, compiled once.
[[nodiscard]] async::task<connected_websocket> connect_websocket_through(connector_handle connector, origin server,
                                                                         websocket_handshake handshake,
                                                                         websocket_connect_options options);

} // namespace detail

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
