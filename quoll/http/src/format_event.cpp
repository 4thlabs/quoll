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

#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/format_event.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::http {

namespace {

// Text that goes in one field: UTF-8, on one line.
void check_field(std::string_view text, std::string_view what) {
  if (!detail::is_utf8(text)) {
    throw std::invalid_argument{std::string{what} + " is not UTF-8"};
  }

  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument{std::string{what} + " holds a line break"};
  }
}

void append_field(std::string &out, std::string_view name, std::string_view value) {
  out.append(name);
  out.append(": ");
  out.append(value);
  out.push_back('\n');
}

} // namespace

std::string format_event(server_sent_event const &event) {
  check_field(event.type, "quoll::http::format_event: the type");

  if (event.id) {
    check_field(*event.id, "quoll::http::format_event: the id");

    if (event.id->contains('\0')) {
      throw std::invalid_argument{"quoll::http::format_event: the id holds a NUL, which a reader ignores"};
    }

    if (event.id->size() > max_event_id_size) {
      throw std::invalid_argument{"quoll::http::format_event: the id is longer than a reader takes"};
    }
  }

  if (!detail::is_utf8(event.data)) {
    throw std::invalid_argument{"quoll::http::format_event: the data is not UTF-8"};
  }

  std::string out;

  // Mostly one allocation: the fields' names and line ends, about a byte in eight of data, and the rest.
  out.reserve(event.type.size() + (event.id ? event.id->size() : 0) + event.data.size() + (event.data.size() / 8) + 32);

  if (!event.type.empty() && event.type != "message") {
    append_field(out, "event", event.type);
  }

  if (event.id) {
    append_field(out, "id", *event.id);
  }

  std::string_view const data = event.data;

  for (std::size_t position = 0;;) {
    auto const end = data.find_first_of("\r\n", position);

    append_field(out, "data", data.substr(position, end - position));

    if (end == std::string_view::npos) {
      break;
    }

    position = end + 1;

    if (data[end] == '\r' && position < data.size() && data[position] == '\n') {
      ++position;
    }
  }

  out.push_back('\n');
  return out;
}

std::string format_retry(std::chrono::milliseconds delay) {
  if (delay.count() < 0) {
    throw std::invalid_argument{"quoll::http::format_retry: a negative time"};
  }

  std::string out = "retry: " + std::to_string(delay.count());

  // No blank line: it would end the event being written, its id and type, if one were written in pieces around it.
  out.push_back('\n');
  return out;
}

std::string format_comment(std::string_view text) {
  check_field(text, "quoll::http::format_comment: the text");

  std::string out = ":";

  if (!text.empty()) {
    out.push_back(' ');
    out.append(text);
  }

  out.push_back('\n');
  return out;
}

} // namespace quoll::http
