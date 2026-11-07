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

#ifndef QUOLL_HTTP_TRANSPORT_HPP
#define QUOLL_HTTP_TRANSPORT_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>

#include <concepts>
#include <utility>

namespace quoll::http {

/**
 * @brief Sends HTTP requests and produces their responses: a client over a connector, or the platform's own HTTP
 * stack (the browser's fetch).
 *
 * A transport is bound to an origin: `request.target` is in origin form (`/path?query`). `send(request)` returns a
 * task producing the response; every response is a result, error statuses included. Whether redirects are followed
 * depends on the transport (fetch follows them). A failure to get one (network
 * error) is reported by the task's exception (std::system_error), and a stop request on the awaiter's context
 * abandons the request, with `std::errc::operation_canceled`. A `request.body_writer` is for the transport to run: in
 * pieces over a connection, or to its end first for what has no use for them.
 *
 * The task owns what it needs: it does not refer to the transport, which may be gone by the time it runs.
 */
template <class T>
concept HttpTransport = requires(T const &transport, request message) {
  { transport.send(std::move(message)) } -> std::same_as<async::task<response>>;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_TRANSPORT_HPP
