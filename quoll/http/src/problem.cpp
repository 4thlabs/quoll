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

#include <quoll/http/problem.hpp>

#include <quoll/http/detail/printable_text.hpp>
#include <quoll/http/detail/utf8.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::http {

namespace {

// `text` as a JSON string, its quotes, backslashes and control characters escaped.
void append_json_string(std::string &out, std::string_view text) {
  out += '"';

  for (auto const character : text) {
    auto const byte = static_cast<unsigned char>(character);

    if (character == '"' || character == '\\') {
      out += '\\';
      out += character;
    } else if (character == '\n') {
      out += "\\n";
    } else if (byte < 0x20) {
      char escaped[8];

      std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(byte));
      out += escaped;
    } else {
      out += character;
    }
  }

  out += '"';
}

} // namespace

response problem_response(status code, std::optional<std::string> said) {
  // It may quote what came from a peer, and an error has to get out.
  if (said && !detail::is_utf8(*said)) {
    said = detail::printable_text(std::move(*said));
  }

  auto const reason = reason_phrase(code);
  std::string body = "{";

  if (!reason.empty()) {
    body += "\"title\":";
    append_json_string(body, reason);
    body += ',';
  }

  body += "\"status\":" + std::to_string(static_cast<int>(code));

  if (said) {
    body += ",\"detail\":";
    append_json_string(body, *said);
  }

  body += '}';

  response answer;

  answer.status = code;
  answer.body = std::move(body);
  answer.fields.add("Content-Type", problem_content_type);
  return answer;
}

} // namespace quoll::http
