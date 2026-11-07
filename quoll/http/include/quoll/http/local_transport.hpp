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

#include <memory>
#include <utility>

namespace quoll::http {

/**
 * @brief HttpTransport handing requests to a router of the same program, without serializing them: in-process
 * calls, and tests of a client against its server.
 *
 * It writes the responses as a connection would: the response to a HEAD request, which the router may take from a
 * GET route, has no body. What a handler or a middleware throws comes out of the task, where a connection answers 500:
 * a test sees the failure itself. Nothing is serialized, so that a field a connection could not write passes
 * here, but a `Content-Length` other than the body answers 500, as there.
 * A `request.body_writer` runs to its end before the handler, which finds what it wrote in `body`.
 * Keeps its own copy of the router, which its tasks share, so that nothing has to outlive them: add the routes first,
 * as one added to `target` afterwards is not served.
 */
class local_transport {
public:
  /** @brief A transport to a copy of `target`. */
  explicit local_transport(router target);

  // Copied rather than moved, which is as cheap: one moved from stays usable, rather than hold a null pointer.
  local_transport(local_transport const &) noexcept = default;
  local_transport &operator=(local_transport const &) noexcept = default;
  ~local_transport() = default;

  /** @brief The response of the router to `message`. Thread safety: from any thread, as copies share a router const. */
  [[nodiscard]] async::task<response> send(request message) const { return send_to(router_, std::move(message)); }

private:
  // Static: the task holds the router, not the transport.
  static async::task<response> send_to(std::shared_ptr<router const> target, request message);

  std::shared_ptr<router const> router_;
};

static_assert(HttpTransport<local_transport>);

} // namespace quoll::http

#endif // QUOLL_HTTP_LOCAL_TRANSPORT_HPP
