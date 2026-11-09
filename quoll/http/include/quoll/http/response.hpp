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

#ifndef QUOLL_HTTP_RESPONSE_HPP
#define QUOLL_HTTP_RESPONSE_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/version.hpp>

#include <functional>
#include <stop_token>
#include <string>

namespace quoll::io {
class any_stream;
} // namespace quoll::io

namespace quoll::http {

/**
 * @brief What takes a connection over once it has switched protocols: its stream, what came on it after the request,
 * and a token stopped when the server drains, which asks it to end gently. See http::response::upgrade.
 */
using upgrade_handler =
    std::function<async::task<>(io::any_stream stream, std::string received, std::stop_token drain)>;

/** @brief A complete HTTP/1.x response: status line, fields, body and trailers. */
struct response {
  http::status status = http::status::ok;

  /** @brief Reason phrase as received; when empty, the serializer writes the standard one. */
  std::string reason{};

  http::version version = http::version::http_1_1;
  http::fields fields{};
  std::string body{};

  /** @brief Fields sent after a chunked body. */
  http::fields trailers{};

  /** @brief Writes the body in pieces, rather than `body`, which stays empty: see http::body_writer. */
  http::body_writer body_writer{};
  /**
   * @brief The handler of a route taking an http::response_writer, set by that route: it gives the status and fields
   * with its first write, and runs for HEAD too. What serves a router runs it, unless `body_writer` is set, which comes
   * first: a middleware replaces it by setting that one, and wraps it by setting this one, with what it writes through
   * passing `head()` on.
   */
  http::body_writer late_head_writer{};
  /**
   * @brief What takes the connection over once this response has left, for a 101 that answers a request asking to
   * switch protocols, as http::accept_websocket sets it: see http::upgrade_handler. What
   * serves a router over a connection runs it, in the task of that connection; a response with it that is not such an
   * answer is replaced with a 500. Nothing else runs it.
   */
  http::upgrade_handler upgrade{};
};

} // namespace quoll::http

#endif // QUOLL_HTTP_RESPONSE_HPP
