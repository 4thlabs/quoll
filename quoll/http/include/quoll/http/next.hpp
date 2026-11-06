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

#ifndef QUOLL_HTTP_NEXT_HPP
#define QUOLL_HTTP_NEXT_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace quoll::http {

class router;

namespace detail {

struct resolved_route;

} // namespace detail

/**
 * @brief What comes after a middleware: the next one, or the route. Called once in general, or not at all to answer in
 * its place. It also says what the router found for the request: routing is done before the first middleware runs.
 *
 * Copied freely, but valid only for as long as the middleware's task, like what it returns: it points to the request's
 * state, so that work going on after the response has to finish before that, rather than keep it.
 *
 * Thread safety: its const functions may be called from any thread, as dispatch() may. What the routes and later
 * middlewares do then is theirs to protect.
 */
class next {
public:
  /** @brief The response to `message`. Anything a handler or a later middleware throws comes out of the task. */
  [[nodiscard]] async::task<response> operator()(request message) const;

  /**
   * @brief The path of the request, percent-decoded and validated, what to take a decision on rather than the target as
   * the peer sent it: `/%61dmin` and `http://x/admin` are both `/admin`. Empty for a target the router answers 400.
   *
   * An encoded slash stays encoded, as it is a character of its segment and no separator: `/a%2Fb` is not `/a/b`, which
   * is another path. A percent sign is encoded too, so that one path means one list of segments. The rest is what the
   * peer sent, control characters included: it is no text to write as it is into a log or a field.
   */
  [[nodiscard]] std::string_view path() const noexcept;

  /** @brief The name of the route that answers, empty if none does (404, 405, 400). */
  [[nodiscard]] std::string_view route() const noexcept;

  /**
   * @brief Once awaited: whether what came back is the router's own answer (404, 405, 400), with no route and no later
   * middleware answering in its place, though one may have changed it on its way back.
   */
  [[nodiscard]] bool router_answered() const noexcept;

  /** @brief The path parameters of that route, percent-decoded, in the order of its template. */
  [[nodiscard]] std::span<std::string const> params() const noexcept;

private:
  friend class router;

  next(router const &owner, detail::resolved_route const &resolved, std::size_t position) noexcept
      : router_(&owner), resolved_(&resolved), position_(position) {}

  router const *router_;
  detail::resolved_route const *resolved_;
  std::size_t position_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_NEXT_HPP
