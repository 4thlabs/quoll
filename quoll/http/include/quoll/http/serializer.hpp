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

#ifndef QUOLL_HTTP_SERIALIZER_HPP
#define QUOLL_HTTP_SERIALIZER_HPP

#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>

#include <string>

namespace quoll::http {

/**
 * @brief Appends `message` to `out`, as sent on the wire.
 *
 * Adds `Content-Length` when the fields declare no framing: to a request with a body, or a POST / PUT / PATCH, and to a
 * response whatever its body, even an empty one, unless its status has none (1xx, 204, 304). With `Transfer-Encoding`
 * ending in `chunked`, the body is written as one chunk followed by the trailers. A `Content-Length` given in the
 * fields must match the body; only a response with an empty body may announce one it does not send (response to HEAD,
 * 304).
 *
 * Every framing a peer could read differently from what is sent is rejected, as it would desync the connection. What
 * is rejected leaves `out` as it was, whose content can still be sent; `out` must not be part of `message`.
 *
 * @throws std::invalid_argument for a target with anything but visible ASCII, a field name that is not a token, a value
 * or a reason phrase with a control character, a status code outside 100-999, a body, trailers or `Transfer-Encoding`
 * on a 1xx / 204 / 304 response, a CONNECT or a TRACE request, a tab in `Transfer-Encoding` or `Content-Length`,
 * `Content-Length` on a 1xx / 204 response, a `Content-Length` that is invalid, repeated or does not match the body,
 * `Content-Length` with `Transfer-Encoding`, a last transfer coding other than chunked, chunked applied twice, chunked
 * coding in HTTP/1.0, trailers without chunked coding, `Content-Length` or `Transfer-Encoding` in the trailers.
 */
void serialize(request const &message, std::string &out);

/** @copydoc serialize(request const &, std::string &) */
void serialize(response const &message, std::string &out);

/** @brief `message` as sent on the wire. @throws std::invalid_argument as serialize(request const &, std::string &). */
[[nodiscard]] std::string serialize(request const &message);

/** @brief `message` as sent on the wire. @throws std::invalid_argument as serialize(request const &, std::string &). */
[[nodiscard]] std::string serialize(response const &message);

} // namespace quoll::http

#endif // QUOLL_HTTP_SERIALIZER_HPP
