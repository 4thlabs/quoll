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

#ifndef QUOLL_HTTP_STREAMED_RESPONSE_HPP
#define QUOLL_HTTP_STREAMED_RESPONSE_HPP

#include <quoll/http/fields.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/version.hpp>

#include <memory>
#include <string>

namespace quoll::http {

/**
 * @brief A response received, whose head has come and whose body is read a piece at a time: what a transport's
 * `stream()` produces, client::transport's among them. A handler writing its response in pieces uses
 * `response.body_writer` instead.
 *
 * @code
 * auto response = co_await transport.stream(std::move(request));
 *
 * while (auto const piece = co_await response.body.read()) {
 *   co_await store(*piece);
 * }
 * @endcode
 *
 * Destroyed before the end of its body, with every copy of `body`, is how to give up on it. A failure to read throws
 * std::system_error from `body.read()`, and reading ends there, as it does on a cancellation: a stream does not take it
 * up again. Each transport says what else holds, in its `stream()`.
 *
 * Thread safety: none.
 */
struct streamed_response {
  http::status status = http::status::ok;
  /** @brief Reason phrase as received. */
  std::string reason{};
  http::version version = http::version::http_1_1;
  http::fields fields{};
  response_body body{};
  /**
   * @brief Fields that came after a chunked body, once `body` has come to its end; null on a response default-built or
   * moved from.
   */
  std::shared_ptr<http::fields const> trailers{};
};

} // namespace quoll::http

#endif // QUOLL_HTTP_STREAMED_RESPONSE_HPP
