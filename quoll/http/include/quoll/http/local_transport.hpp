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

#ifndef QUOLL_HTTP_LOCAL_TRANSPORT_HPP
#define QUOLL_HTTP_LOCAL_TRANSPORT_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/transport.hpp>

#include <functional>
#include <utility>

namespace quoll::http {

/**
 * @brief HttpTransport handing requests to a router of the same program, without serializing them: in-process
 * calls, and tests of a client against its server.
 *
 * It writes the responses as a connection would: the response to a HEAD request, which the router may take from a
 * GET route, has no body. What a handler or a middleware throws comes out of the task, where a connection answers 500:
 * a test sees the failure itself. Nothing is serialized, so that a response a connection could not write passes here.
 * Holds a reference: the router outlives the transport and the tasks it returns.
 */
class local_transport {
public:
  explicit local_transport(router const &target) noexcept : router_(target) {}

  [[nodiscard]] async::task<response> send(request message) const { return send_to(router_, std::move(message)); }

private:
  // Static: the task holds the router reference, not the transport.
  static async::task<response> send_to(std::reference_wrapper<router const> target, request message);

  std::reference_wrapper<router const> router_;
};

static_assert(HttpTransport<local_transport>);

} // namespace quoll::http

#endif // QUOLL_HTTP_LOCAL_TRANSPORT_HPP
