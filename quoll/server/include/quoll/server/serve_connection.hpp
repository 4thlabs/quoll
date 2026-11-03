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

#include <quoll/async/task.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/server/detail/connection.hpp>

#include <string>
#include <utility>

namespace quoll::server {

/**
 * @brief Serves the requests of one connection with `router`, one after the other, until the connection ends.
 *
 * Each request goes to the router; its response is written back before the next request is read (pipelined requests
 * wait in the connection's buffer). The connection ends when the peer closes it, after a response to a request that
 * does not keep it alive (`Connection: close`, HTTP/1.0 without keep-alive, an upgrade, which is not served), on a
 * stream error, or on a stop request, which cancels the pending read.
 *
 * - A request that is not valid HTTP answers 400, one with too large a header 431, too large a body 413, an unknown
 *   method 501, and ends the connection. `limits` bound what a request may hold.
 * - A handler that throws answers 500, as does a response that cannot be serialized.
 * - The response to HEAD carries no body: it announces the length of the one the GET route produced.
 *
 * Owns `stream`; `router` outlives the task.
 */
template <io::ByteStream Stream>
async::task<> serve_connection(Stream stream, http::router const &router, http::parse_limits limits = {}) {
  std::string buffer;
  http::request_parser parser{limits};

  for (;;) {
    if (auto const error = co_await http::read(stream, buffer, parser)) {
      if (auto const answer = detail::read_error_status(error)) {
        auto rejected = detail::status_response(*answer);

        detail::prepare(rejected, false, false);
        (void)co_await detail::write_response(stream, rejected, false);
      }

      co_return;
    }

    auto const keep_alive = parser.keep_alive() && !parser.upgrade();
    auto request = parser.release();
    auto const head = request.method == http::verb::head;
    auto response = co_await detail::respond(router, std::move(request));

    detail::prepare(response, head, keep_alive);

    if (co_await detail::write_response(stream, response, keep_alive) || !keep_alive) {
      co_return;
    }
  }
}

} // namespace quoll::server

#endif // QUOLL_SERVER_SERVE_CONNECTION_HPP
