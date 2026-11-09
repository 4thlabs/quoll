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

#ifndef QUOLL_CLIENT_WEBSOCKET_CONNECT_OPTIONS_HPP
#define QUOLL_CLIENT_WEBSOCKET_CONNECT_OPTIONS_HPP

#include <quoll/http/fields.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/io/tls_options.hpp>

#include <chrono>

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
  /**
   * @brief What the connection accepts, and its `ping_interval`: how long the server may send nothing, while a
   * receive() reads, before it is pinged, then before it is taken as gone, and how long a write may wait with nothing
   * taken.
   */
  websocket_options websocket{};
  /** @brief How long connecting and the handshake may take, both, given a timer; zero: no limit, and a negative one has
   * expired already, as with_timeout() has it. */
  std::chrono::nanoseconds handshake_timeout = std::chrono::seconds{30};
  /** @brief Size limits of the server's answer to the handshake. */
  http::parse_limits limits{};
  /** @brief How `wss` checks the server: by default, against the system's authorities. */
  io::tls_client_options tls{};
};

} // namespace quoll::http

#endif // QUOLL_CLIENT_WEBSOCKET_CONNECT_OPTIONS_HPP
