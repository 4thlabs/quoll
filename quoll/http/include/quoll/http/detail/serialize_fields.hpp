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

#ifndef QUOLL_HTTP_DETAIL_SERIALIZE_FIELDS_HPP
#define QUOLL_HTTP_DETAIL_SERIALIZE_FIELDS_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/fields.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::http::detail {

inline void serialize_fields(fields const &entries, std::string &out) {
  for (auto const &entry : entries) {
    if (!is_token(entry.name) || has_line_break(entry.value)) {
      throw std::invalid_argument{"quoll::http: invalid field " + entry.name};
    }

    out += entry.name;
    out += ": ";
    out += entry.value;
    out += "\r\n";
  }
}

// RFC 9112: chunked is the last transfer coding when present.
[[nodiscard]] inline bool is_chunked(fields const &entries) noexcept {
  auto const coding = entries.find("Transfer-Encoding");

  if (!coding) {
    return false;
  }

  auto const value = trim(*coding);
  constexpr std::string_view chunked = "chunked";

  return value.size() >= chunked.size() && iequals(value.substr(value.size() - chunked.size()), chunked);
}

inline void append_hex(std::size_t value, std::string &out) {
  std::array<char, 2 * sizeof(std::size_t)> digits{};
  auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);

  static_cast<void>(error);
  out.append(digits.data(), end);
}

// Fields, then the body framed by Content-Length (added when `has_body` and the fields declare no framing) or by
// chunked transfer coding, with the trailers.
inline void serialize_framing(fields const &entries, std::string const &body, fields const &trailers, bool has_body,
                              std::string &out) {
  serialize_fields(entries, out);

  if (is_chunked(entries)) {
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

  if (!trailers.empty()) {
    throw std::invalid_argument{"quoll::http: trailers need Transfer-Encoding: chunked"};
  }

  if (has_body && !entries.contains("Content-Length") && !entries.contains("Transfer-Encoding")) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  out += "\r\n";
  out += body;
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_SERIALIZE_FIELDS_HPP
