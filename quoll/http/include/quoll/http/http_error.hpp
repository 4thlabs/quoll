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

#ifndef QUOLL_HTTP_HTTP_ERROR_HPP
#define QUOLL_HTTP_HTTP_ERROR_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace quoll::http {

/**
 * @brief An HTTP error: raised by a handler, which the server answers with its status, or received, from a response
 * another server sent, which a client raises.
 *
 * @code
 * router.get("/notes/{id}", [](std::int64_t id) -> http::response {
 *   throw http::http_error{http::status::not_found, "no note " + std::to_string(id)};
 * });
 * @endcode
 *
 * The server answers one its handler raises with its status and an RFC 9457 problem whose detail is what(). One
 * received from another call, let through by a handler, is answered `502 Bad Gateway`: the other server's status and
 * detail are its own, not this one's. Copies and no moves: an exception does not throw on its way to a handler.
 */
class http_error : public std::runtime_error {
public:
  /** @brief Raised: the server answers `code`, with `detail`. */
  http_error(http::status code, std::string const &detail) : std::runtime_error(detail), status_(code) {}

  /** @brief Received: `answer`, a response another server sent, `detail` saying what went wrong. */
  [[nodiscard]] static http_error received_from(http::response answer, std::string const &detail);

  http_error(http_error const &) noexcept = default;
  http_error &operator=(http_error const &) noexcept = default;
  ~http_error() override = default;

  /** @brief Its status: the one to answer, or the one received. */
  [[nodiscard]] http::status status() const noexcept { return status_; }

  /** @brief Whether it comes from a response received rather than from this program's code. */
  [[nodiscard]] bool received() const noexcept { return response_ != nullptr; }

  /** @brief The response received, its fields and body as they came; none when it was raised. */
  [[nodiscard]] http::response const *answer() const noexcept QUOLL_LIFETIMEBOUND { return response_.get(); }

private:
  http::status status_;
  std::shared_ptr<http::response const> response_;
};

/**
 * @brief `answer` if its status is not an error, else an http_error received from it, its detail the status and its
 * reason: `co_await client.send(request)` read as `http::ensure_ok(co_await client.send(request))`.
 * @throws http_error for a status of 400 or more.
 */
[[nodiscard]] response ensure_ok(response answer);

} // namespace quoll::http

#endif // QUOLL_HTTP_HTTP_ERROR_HPP
