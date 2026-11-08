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

#ifndef QUOLL_REST_RETRY_HPP
#define QUOLL_REST_RETRY_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/rest/next.hpp>

#include <chrono>

namespace quoll::rest {

/**
 * @brief An interceptor (see client::use) sending a request again when it may have failed on the way rather than
 * at the server: after a network error (a reset, a refused connection, a timeout, a connection ended in the middle of
 * the response), or a 502, 503 or 504. An error of the message itself (too large, too many redirects) is no reason. Up
 * to `attempts` in all, waiting `delay` before the second, then twice as long each time.
 *
 * Only a request that may be sent twice with the same effect is: GET, HEAD, PUT, DELETE, OPTIONS and TRACE (RFC 9110,
 * 9.2.2), with no `body_writer`, which runs once. A stop request is no failure: it ends the call, between attempts
 * too. The wait
 * needs the timer of the executor that calls (see async::sleep_for); with a `delay` of zero, there is none.
 *
 * @code
 * api.use(quoll::rest::retry{.attempts = 3, .delay = std::chrono::milliseconds{200}});
 * @endcode
 */
struct retry {
  int attempts = 3;
  std::chrono::milliseconds delay{100};

  [[nodiscard]] async::task<http::response> operator()(http::request &&message, next const &send) const;
};

} // namespace quoll::rest

#endif // QUOLL_REST_RETRY_HPP
