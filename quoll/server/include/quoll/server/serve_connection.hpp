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

#ifndef QUOLL_SERVER_SERVE_CONNECTION_HPP
#define QUOLL_SERVER_SERVE_CONNECTION_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/server/connection_options.hpp>

#include <concepts>
#include <utility>

namespace quoll::server {

/**
 * @brief Serves the requests of one connection with `router`, one after the other, until the connection ends.
 *
 * Each request goes to the router; its response is written back before the next request is read (pipelined requests
 * wait in the connection's buffer). The connection ends when the peer closes it, after a response to a request that
 * does not keep it alive (`Connection: close`, HTTP/1.0 without keep-alive, an upgrade, which is not served), after a
 * response whose handler set `Connection: close`, on a stream error, on `options.drain`, gently, or on a stop request,
 * which cancels whatever is pending: a read, a handler's own operations, a write. Timeouts end it too (see
 * connection_options): a connection idle for too long, a request taking too long to arrive (408), a response taking too
 * long to leave.
 *
 * - A request that is not valid HTTP answers 400, one with too large a header 431, too large a body 413, as soon as its
 *   head announces it, an unknown method 501, an `Expect` field asking for anything but `100-continue` 417, and ends
 *   the connection.
 * - A request with `Expect: 100-continue` whose body has not come gets `100 Continue`, then the body is read.
 * - A handler or a middleware that throws answers 500 (and is reported to `options.on_handler_error`), as does a
 * response that cannot be serialized.
 * - The response to HEAD carries no body: it announces the length of the one the GET route produced.
 *
 * Ending a connection stops sending first, with `stream.shutdown_send()`: a TLS peer gets its closing message. After
 * the last response, what the peer still sends is then read and dropped, as `options.lingering_timeout` and
 * `options.lingering_time` say, unless `options.drain` or a stop request ends it.
 *
 * Owns `stream`; `router` and `options` outlive the task.
 */
async::task<> serve_connection(io::any_stream stream, http::router const &router QUOLL_LIFETIMEBOUND,
                               connection_options const &options QUOLL_LIFETIMEBOUND);

/** @brief serve_connection() with the default options. */
async::task<> serve_connection(io::any_stream stream, http::router const &router QUOLL_LIFETIMEBOUND);

/** @brief serve_connection() on a stream an io::any_stream can hold. */
template <io::ByteStream Stream>
  requires std::constructible_from<io::any_stream, Stream>
async::task<> serve_connection(Stream stream, http::router const &router QUOLL_LIFETIMEBOUND,
                               connection_options const &options QUOLL_LIFETIMEBOUND) {
  return serve_connection(io::any_stream{std::move(stream)}, router, options);
}

/** @brief serve_connection() on a stream an io::any_stream can hold, with the default options. */
template <io::ByteStream Stream>
  requires std::constructible_from<io::any_stream, Stream>
async::task<> serve_connection(Stream stream, http::router const &router QUOLL_LIFETIMEBOUND) {
  return serve_connection(io::any_stream{std::move(stream)}, router);
}

} // namespace quoll::server

#endif // QUOLL_SERVER_SERVE_CONNECTION_HPP
