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

#ifndef QUOLL_CLIENT_TRANSPORT_OPTIONS_HPP
#define QUOLL_CLIENT_TRANSPORT_OPTIONS_HPP

#include <quoll/http/parser.hpp>
#include <quoll/io/tls_options.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace quoll::client {

/**
 * @brief Settings of a client::transport.
 *
 * The timeouts end the request with std::system_error (std::errc::timed_out). Zero disables one. They need the timer of
 * the executor that sends, as quoll::runtime provides it: on an executor without one (a thread pool), they do not
 * apply.
 */
struct transport_options {
  /**
   * @brief Size limits of the responses, against servers sending unbounded ones. A body through transport::stream(),
   * that memory does not hold and the caller is reading, has none by default.
   */
  http::parse_limits limits{.max_streamed_body_size = std::numeric_limits<std::uint64_t>::max()};
  /**
   * @brief Connections kept open once their response is read, for the next requests (keep-alive); 0 closes each
   * connection after its response.
   */
  std::size_t max_idle_connections = 8;
  /**
   * @brief How many connections are open at once, in use or idle; 0: no limit. Past it, a request waits for a request
   * on a connection to end, and an idle connection is closed to make room for a new one. A streamed_response counts
   * until its body has come to its end or it goes: read or drop one before a request that needs its place. A stop
   * request ends the wait, and so does `connect_timeout`, which then starts again to connect. Without a timeout or a
   * timer, a server that does not answer holds up those that wait. The executors that send have to take a post() from
   * another thread, as asio's and a thread pool's do, and a run_loop's does not.
   */
  std::size_t max_connections = 0;
  /**
   * @brief How many redirects a request follows; 0: a 3xx response is a result like the others. Past it, the request
   * fails with std::system_error (`http::error::too_many_redirects`). Only to the transport's own server, in the same
   * scheme; see client::transport.
   */
  std::size_t max_redirects = 0;
  /** @brief How long opening a connection may take, name resolution included, and waiting for `max_connections`. */
  std::chrono::nanoseconds connect_timeout = std::chrono::seconds{30};
  /**
   * @brief How long a request may take once on a connection, from sending it to reading the end of its response: a
   * connection found closed and tried again counts anew. Its connection is then closed; the request is not tried again,
   * as the server may be working on it. Through transport::stream(), it is up to the head, then per piece: a transfer
   * takes as long as it keeps coming.
   */
  std::chrono::nanoseconds response_timeout = std::chrono::seconds{60};
  /** @brief How the servers of `https://` URLs are checked: by default, against the system's authorities. */
  io::tls_client_options tls{};
};

} // namespace quoll::client

#endif // QUOLL_CLIENT_TRANSPORT_OPTIONS_HPP
