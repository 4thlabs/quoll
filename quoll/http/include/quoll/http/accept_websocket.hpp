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

#ifndef QUOLL_HTTP_ACCEPT_WEBSOCKET_HPP
#define QUOLL_HTTP_ACCEPT_WEBSOCKET_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/websocket.hpp>

#include <functional>

namespace quoll::http {

/**
 * @brief The answer to a request for a WebSocket: a 101, after which `handler` takes the connection over as a server's
 * http::websocket, for as long as its task runs.
 *
 * @code
 * router.get("/echo", [](http::request request) {
 *   return http::accept_websocket(request, [](http::websocket socket) -> async::task<> {
 *     for (;;) {
 *       auto message = co_await socket.receive();
 *
 *       if (message.opcode == http::websocket_opcode::close) {
 *         co_return;
 *       }
 *
 *       co_await socket.send_text(message.data);
 *     }
 *   });
 * });
 * @endcode
 *
 * A request that is no WebSocket handshake (see is_websocket_upgrade()) is answered 400, or 426 with
 * `Sec-WebSocket-Version: 13` if it asks for another version, and the handler never runs. The response is the route's
 * to complete: a subprotocol the request offers in `Sec-WebSocket-Protocol`, chosen in the fields, or a status of its
 * own, which then refuses the handshake.
 *
 * The handler runs in the task of the connection, as a request's handler does: stopping a server waits for it up to
 * its `shutdown_timeout`, then stops what it awaits. Its exceptions go to `connection_options::on_handler_error`. The
 * timeouts of the connection no longer apply, as a WebSocket may stay quiet: see http::websocket.
 *
 * @throws std::invalid_argument for an empty handler.
 */
[[nodiscard]] response accept_websocket(request const &message, std::function<async::task<>(websocket)> handler,
                                        websocket_options options = {});

} // namespace quoll::http

#endif // QUOLL_HTTP_ACCEPT_WEBSOCKET_HPP
