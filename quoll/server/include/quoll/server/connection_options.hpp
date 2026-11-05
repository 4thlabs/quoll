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

#ifndef QUOLL_SERVER_CONNECTION_OPTIONS_HPP
#define QUOLL_SERVER_CONNECTION_OPTIONS_HPP

#include <quoll/http/parser.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <stop_token>

namespace quoll::server {

/**
 * @brief How serve_connection() reads requests and reports what its handlers throw.
 *
 * The timeouts protect a server from peers that are slow or gone. Zero disables one. They need the timer of the
 * connection's executor, as quoll::runtime provides it: on an executor without one (a thread pool), they do not apply.
 * The time a handler or a middleware takes is not limited: it is the application's to bound, with async::with_timeout.
 */
struct connection_options {
  /** @brief What a request may hold: beyond, it answers 431 (header) or 413 (body). */
  http::parse_limits limits{};

  /** @brief How long a connection waits for a request to start. Past it, the connection closes without a word. */
  std::chrono::nanoseconds idle_timeout = std::chrono::seconds{60};

  /**
   * @brief How long a request may take to arrive once it has started, as a whole: a peer sending a byte now and then
   * gets no more. Past it, it answers 408 and the connection closes. It starts again once `100 Continue` has left, as
   * the client waits for that. A body read in pieces (http::body_reader) has it per piece: a transfer takes as long as
   * it keeps coming, so that only a route that asks for it lets a slow peer stay.
   */
  std::chrono::nanoseconds read_timeout = std::chrono::seconds{30};

  /**
   * @brief How long a response or a `100 Continue` may take to leave, as a whole, and each piece of a response written
   * in pieces (http::body_writer). Past it, the connection closes.
   */
  std::chrono::nanoseconds write_timeout = std::chrono::seconds{30};

  /**
   * @brief After the last response of a connection, once sending has stopped, what the peer still sends is read and
   * dropped, as closing with its bytes unread would reset the connection, and may destroy the response before it reads
   * it. This is how long to wait for more of it. Zero, or no timer, closes at once.
   */
  std::chrono::nanoseconds lingering_timeout = std::chrono::seconds{5};

  /** @brief How long this goes on as a whole, against a peer that keeps sending. Zero closes at once. */
  std::chrono::nanoseconds lingering_time = std::chrono::seconds{30};

  /**
   * @brief A stop request here ends the connection gently: an idle one at once, one with a request under way once its
   * response has left, saying `Connection: close` if the request came before the response started. A stop request on
   * the task cancels whatever is pending instead. serve() sets it for its own shutdown, replacing what is given here.
   */
  std::stop_token drain{};

  /**
   * @brief Called with what a handler, a middleware or a body_writer threw, or for a writer short of its `Content-Length`; on
   * the connection's executor, must not throw. The client receives a 500 while nothing of the response has left, and
   * otherwise a body cut short. Empty: that is all that is left of it.
   */
  std::function<void(std::exception_ptr)> on_handler_error{};
};

} // namespace quoll::server

#endif // QUOLL_SERVER_CONNECTION_OPTIONS_HPP
