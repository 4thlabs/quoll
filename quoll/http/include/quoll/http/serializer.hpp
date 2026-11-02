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

#include <quoll/http/detail/serialize_fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::http {

/**
 * @brief Appends `message` to `out`, as sent on the wire.
 *
 * Adds `Content-Length` when the message has a body (or, for POST / PUT / PATCH, an empty one) and the fields
 * declare no framing. With `Transfer-Encoding: chunked`, the body is written as one chunk followed by the trailers;
 * trailers without it are an error.
 *
 * @throws std::invalid_argument for what cannot be sent: a target with spaces or line breaks, a field name that is
 * not a token, a value or a reason phrase with a line break, a status code outside 100-999.
 */
inline void serialize(request const &message, std::string &out) {
  if (message.target.empty() || message.target.find(' ') != std::string::npos ||
      detail::has_line_break(message.target)) {
    throw std::invalid_argument{"quoll::http: invalid request target"};
  }

  out += to_string(message.method);
  out += ' ';
  out += message.target;
  out += ' ';
  out += to_string(message.version);
  out += "\r\n";

  auto const method_has_body =
      message.method == verb::post || message.method == verb::put || message.method == verb::patch;

  detail::serialize_framing(message.fields, message.body, message.trailers, !message.body.empty() || method_has_body,
                            out);
}

/** @copydoc serialize(request const &, std::string &) */
inline void serialize(response const &message, std::string &out) {
  auto const code = static_cast<std::uint16_t>(message.status);

  if (code < 100 || code > 999) {
    throw std::invalid_argument{"quoll::http: invalid status code"};
  }

  if (detail::has_line_break(message.reason)) {
    throw std::invalid_argument{"quoll::http: invalid reason phrase"};
  }

  out += to_string(message.version);
  out += ' ';
  out += std::to_string(code);
  out += ' ';
  out += message.reason.empty() ? reason_phrase(message.status) : std::string_view{message.reason};
  out += "\r\n";

  // RFC 9110: 1xx, 204 and 304 responses never have a body.
  auto const has_body = !is_informational(message.status) && message.status != status::no_content &&
                        message.status != status::not_modified;

  detail::serialize_framing(message.fields, message.body, message.trailers, has_body, out);
}

/** @brief `message` as sent on the wire. @copydetails serialize(request const &, std::string &) */
[[nodiscard]] inline std::string serialize(request const &message) {
  std::string out;

  serialize(message, out);
  return out;
}

/** @brief `message` as sent on the wire. @copydetails serialize(request const &, std::string &) */
[[nodiscard]] inline std::string serialize(response const &message) {
  std::string out;

  serialize(message, out);
  return out;
}

} // namespace quoll::http

#endif // QUOLL_HTTP_SERIALIZER_HPP
