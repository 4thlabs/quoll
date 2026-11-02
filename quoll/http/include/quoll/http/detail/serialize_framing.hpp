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

#ifndef QUOLL_HTTP_DETAIL_SERIALIZE_FRAMING_HPP
#define QUOLL_HTTP_DETAIL_SERIALIZE_FRAMING_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/serialize_fields.hpp>
#include <quoll/http/fields.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace quoll::http::detail {

// What the start line allows: the body framing rules of RFC 9110 / 9112 for this message.
struct framing_rules {
  // False for 1xx, 204 and 304 responses.
  bool body_allowed = true;
  // False for 1xx and 204 responses.
  bool length_allowed = true;
  // Content-Length is added when the fields declare no framing.
  bool add_length = false;
  // False for HTTP/1.0, which has no chunked coding.
  bool chunked_allowed = true;
  // Responses: Content-Length may announce a body that is not sent (HEAD, 304).
  bool length_without_body = false;
};

inline void reject(char const *what) { throw std::invalid_argument{std::string{"quoll::http: "} + what}; }

// Transfer-Encoding fields form one list (RFC 9110, 5.3): true when its last coding is chunked, nothing when there is
// no Transfer-Encoding at all.
[[nodiscard]] inline std::optional<bool> last_coding_is_chunked(fields const &entries) {
  std::optional<bool> chunked;

  for (auto const &entry : entries) {
    if (!iequals(entry.name, "Transfer-Encoding")) {
      continue;
    }

    std::string_view const list = entry.value;
    auto const last =
        trim(list.substr(list.find_last_of(',') == std::string_view::npos ? 0 : list.find_last_of(',') + 1));

    chunked = iequals(last, "chunked");
  }

  return chunked;
}

// The value of the Content-Length fields, nothing when there is none.
[[nodiscard]] inline std::optional<std::size_t> declared_length(fields const &entries) {
  std::optional<std::size_t> length;

  for (auto const &entry : entries) {
    if (!iequals(entry.name, "Content-Length")) {
      continue;
    }

    auto const text = trim(entry.value);
    std::size_t value = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
      reject("invalid Content-Length");
    }

    if (length && *length != value) {
      reject("Content-Length fields disagree");
    }

    length = value;
  }

  return length;
}

inline void append_hex(std::size_t value, std::string &out) {
  std::array<char, 2 * sizeof(std::size_t)> digits{};
  auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);

  static_cast<void>(error);
  out.append(digits.data(), end);
}

// Fields, then the body framed by Content-Length or by chunked coding with the trailers. Every framing the peer
// could read differently from what is sent is rejected: it would desync the connection, or smuggle a request.
inline void serialize_framing(fields const &entries, std::string const &body, fields const &trailers,
                              framing_rules const &rules, std::string &out) {
  auto const chunked = last_coding_is_chunked(entries);
  auto const length = declared_length(entries);

  if (!rules.body_allowed && (!body.empty() || !trailers.empty() || chunked)) {
    reject("this response cannot have a body");
  }

  if (chunked && !*chunked) {
    reject("the last transfer coding must be chunked");
  }

  if (chunked && !rules.chunked_allowed) {
    reject("HTTP/1.0 has no chunked coding");
  }

  if (length && (chunked || !rules.length_allowed)) {
    reject("unexpected Content-Length");
  }

  if (length && *length != body.size() && !(rules.length_without_body && body.empty())) {
    reject("Content-Length does not match the body");
  }

  if (!chunked && !trailers.empty()) {
    reject("trailers need Transfer-Encoding: chunked");
  }

  serialize_fields(entries, out);

  if (chunked) {
    out += "\r\n";

    if (!body.empty()) {
      append_hex(body.size(), out);
      out += "\r\n";
      out += body;
      out += "\r\n";
    }

    out += "0\r\n";
    serialize_fields(trailers, out);
    out += "\r\n";
    return;
  }

  if (rules.add_length && !length) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  out += "\r\n";
  out += body;
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_SERIALIZE_FRAMING_HPP
