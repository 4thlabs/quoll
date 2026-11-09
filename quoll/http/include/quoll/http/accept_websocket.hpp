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
 * to complete, with a subprotocol the request offers in `Sec-WebSocket-Protocol`, chosen in the fields. A route that
 * refuses a handshake, for want of credentials say, returns a response of its own instead: one that hands the
 * connection over with a status other than 101 is answered 500. A handshake that comes as the server drains is answered
 * 503. A browser sends its cookies with a handshake whatever page asks for it, and holds no WebSocket to the same
 * origin: a route that knows its users by a cookie checks the request's `Origin` against its own before accepting, or a
 * page of another site acts for them (cross-site WebSocket hijacking).
 *
 * The handler runs in the task of the connection, on its strand, as a request's handler does. The timeouts of the
 * connection no longer apply, as a WebSocket may stay quiet: a peer quiet for websocket_options::ping_interval, 30
 * seconds by default, is pinged, and one that stays so for another, or does not take a ping within it, gone or
 * half-open, has its handler stopped, which frees the connection. That needs the handler to keep receiving, which is
 * also what answers the peer: one that waits on something else for longer than an interval receives beside it, through
 * when_all, or sets a longer ping_interval. A task left holding a copy of the websocket past the handler keeps the
 * connection open, out of the keep-alive's watch and the server's count, until it lets go. Once a close has left, the
 * handler's own or a drain's, the peer's answer is waited for as a pong is: one that does not come within
 * `ping_interval`, or 30 seconds where it is zero, has the handler stopped. Stopping a server sends a close with 1001
 * (going away) as it starts to drain, which a handler that receives sees as the last message once the peer has answered
 * it, then waits for the handler up to its `shutdown_timeout`, and stops what it awaits.
 *
 * Once the handler is over, a close goes, unless one did: 1000, or 1011 if the handler threw. What the peer sends is
 * then read up to its close, and the server's side shut, so that the close is not lost to a reset, each step within
 * `ping_interval`, or 30 seconds where it is zero. A peer that broke the protocol has had its close: the server's side
 * is shut at once, and what the peer still sends dropped for as long. Pinging, the 1001 of a drain and that wait need a
 * timer, which quoll::runtime has.
 *
 * What the handler throws goes to `connection_options::on_handler_error`, but for what a peer gone, silent, or breaking
 * the protocol throws through it, and for a send refused after a close the handler did not send, the peer's or a
 * drain's: none is a failure of the handler's.
 *
 * @throws std::invalid_argument for an empty handler.
 */
[[nodiscard]] response accept_websocket(request const &message, std::function<async::task<>(websocket)> handler,
                                        websocket_options options = {});

} // namespace quoll::http

#endif // QUOLL_HTTP_ACCEPT_WEBSOCKET_HPP
