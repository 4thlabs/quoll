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

#ifndef QUOLL_REST_CALL_ERROR_HPP
#define QUOLL_REST_CALL_ERROR_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::rest {

/**
 * @brief Thrown by a client's call when the response is not the endpoint's first declared one: another declared
 * response (read it with try_call), an undeclared status, a body that does not decode.
 *
 * what() is the detail of the server's problem when it sent one, else why the response is unexpected. Not an
 * http_error: a service that lets a failed call to another API escape answers 500, not that API's status.
 */
class call_error : public std::runtime_error {
public:
  call_error(unexpected_response failure, std::string const &detail)
      : std::runtime_error(detail), failure_(std::make_shared<unexpected_response const>(std::move(failure))) {}

  // Copies and no moves: an exception does not throw on its way to a handler, and one moved from stays whole.
  call_error(call_error const &) noexcept = default;
  call_error &operator=(call_error const &) noexcept = default;
  ~call_error() override = default;

  /** @brief The status of the response received. */
  [[nodiscard]] http::status status() const noexcept { return failure_->response.status; }

  /** @brief The response received, and why it is unexpected. */
  [[nodiscard]] unexpected_response const &failure() const noexcept QUOLL_LIFETIMEBOUND { return *failure_; }

private:
  std::shared_ptr<unexpected_response const> failure_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_CALL_ERROR_HPP
