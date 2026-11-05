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

#include <quoll/http/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <stop_token>
#include <string_view>

namespace quoll::server {

// The identifier of the next connection a server accepts.
[[maybe_unused]] static std::uint64_t next_connection_id = 0;

/** @brief What on_request() reports of a request that was answered. */
struct request_summary {
  /** @brief The connection it came on: they are numbered in the order they start. */
  std::uint64_t connection = 0;
  /** @brief The request's target; valid during the call. */
  std::string_view target;
  http::status status = http::status::ok;
};

/**
 * @brief How serve_connection() reads requests and reports what its handlers throw.
 *
 * The timeouts protect a server from peers that are slow or gone. Zero disables one. They need the timer of the
 * connection's executor, as quoll::runtime provides it: on an executor without one (a thread pool), they do not apply.
 * The time a handler takes is not limited: it is the application's to bound, with async::with_timeout.
 */
struct connection_options {
  /** @brief What a request may hold: beyond, it answers 431 (header) or 413 (body). */
  http::parse_limits limits{};

  /** @brief How long a connection waits for a request to start. Past it, the connection closes without a word. */
  std::chrono::nanoseconds idle_timeout = std::chrono::seconds{60};

  /**
   * @brief How long a request may take to arrive once it has started, as a whole: a peer sending a byte now and then
   * gets no more. Past it, it answers 408 and the connection closes.
   */
  std::chrono::nanoseconds read_timeout = std::chrono::seconds{30};

  /** @brief How long a response may take to leave, as a whole. Past it, the connection closes. */
  std::chrono::nanoseconds write_timeout = std::chrono::seconds{30};

  /**
   * @brief A stop request here ends the connection gently: an idle one at once, one with a request under way once its
   * response has left, saying `Connection: close` if the request came before the response started. A stop request on
   * the task cancels whatever is pending instead. serve() sets it for its own shutdown, replacing what is given here.
   */
  std::stop_token drain{};

  /**
   * @brief Called with what a handler threw, which the client receives as a 500; on the connection's executor, must
   * not throw. Empty: the 500 is all that is left of it.
   */
  std::function<void(std::exception_ptr)> on_handler_error{};

  /** @brief Called once a response has left, for an access log; on the connection's executor. It must not throw. */
  std::function<void(request_summary const &)> on_request{};

  /** @brief At most this many requests on a connection, whose last response says `Connection: close`. 0: no limit. */
  std::size_t max_requests = 0;
};

} // namespace quoll::server

#endif // QUOLL_SERVER_CONNECTION_OPTIONS_HPP
