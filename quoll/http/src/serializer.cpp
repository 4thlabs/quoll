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

#include <quoll/http/serializer.hpp>

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/check.hpp>
#include <quoll/http/detail/streamed_head.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace quoll::http {

namespace detail {

void check_target(std::string_view target) {
  if (target.empty() || target.find(' ') != std::string_view::npos || has_line_break(target)) {
    throw std::invalid_argument{"quoll::http: invalid request target"};
  }
}

void check_field(field const &entry) {
  if (!is_token(entry.name) || has_line_break(entry.value)) {
    throw std::invalid_argument{"quoll::http: invalid field " + entry.name};
  }
}

} // namespace detail

namespace {

using namespace detail;

void serialize_fields(fields const &entries, std::string &out) {
  for (auto const &entry : entries) {
    out += entry.name;
    out += ": ";
    out += entry.value;
    out += "\r\n";
  }
}

// What the start line allows: the body framing rules of RFC 9110 / 9112 for this message.
struct framing_rules {
  // False for 1xx, 204 and 304 responses, and for a CONNECT request.
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

void reject(char const *what) { throw std::invalid_argument{std::string{"quoll::http: "} + what}; }

// Transfer-Encoding fields form one list (RFC 9110, 5.3): true when its last coding is chunked, nothing when there is
// no Transfer-Encoding at all.
[[nodiscard]] std::optional<bool> last_coding_is_chunked(fields const &entries) {
  std::optional<bool> chunked;

  for (auto const &entry : entries) {
    if (!iequals(entry.name, "Transfer-Encoding")) {
      continue;
    }

    // Whitespace around a coding is allowed, but parsers do not agree on a tab: llhttp reads "chunked\t" as another
    // coding.
    if (entry.value.contains('\t')) {
      reject("a tab in Transfer-Encoding");
    }

    std::string_view const list = entry.value;
    auto const last =
        trim(list.substr(list.find_last_of(',') == std::string_view::npos ? 0 : list.find_last_of(',') + 1));

    chunked = iequals(last, "chunked");
  }

  return chunked;
}

// The value of the Content-Length fields, nothing when there is none.
[[nodiscard]] std::optional<std::size_t> declared_length(fields const &entries) {
  std::optional<std::size_t> length;

  for (auto const &entry : entries) {
    if (!iequals(entry.name, "Content-Length")) {
      continue;
    }

    if (entry.value.contains('\t')) {
      reject("a tab in Content-Length");
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

void append_hex(std::size_t value, std::string &out) {
  std::array<char, 2 * sizeof(std::size_t)> digits{};
  auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);

  static_cast<void>(error);
  out.append(digits.data(), end);
}

void check_fields(fields const &entries) {
  for (auto const &entry : entries) {
    check_field(entry);
  }
}

// How a message that passed its checks goes out.
struct framing {
  bool chunked = false;
  bool add_length = false;
};

// Every framing the peer could read differently from what is sent is rejected: it would desync the connection, or
// smuggle a request. Nothing is written before this passes, so that what is refused leaves nothing behind.
[[nodiscard]] framing check_framing(fields const &entries, std::string const &body, fields const &trailers,
                                    framing_rules const &rules) {
  check_fields(entries);
  check_fields(trailers);

  // RFC 9110, 6.5.1: a peer merging its trailers into the fields would read another framing.
  for (auto const &entry : trailers) {
    if (iequals(entry.name, "Content-Length") || iequals(entry.name, "Transfer-Encoding")) {
      reject("framing in the trailers");
    }
  }

  auto const chunked = last_coding_is_chunked(entries);
  auto const length = declared_length(entries);

  if (!rules.body_allowed && (!body.empty() || !trailers.empty() || chunked)) {
    reject("this message cannot have a body");
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

  return {.chunked = chunked.has_value(), .add_length = rules.add_length && !length};
}

// Fields, then the body framed by Content-Length or by chunked coding with the trailers.
void serialize_framing(fields const &entries, std::string const &body, fields const &trailers, framing const &checked,
                       std::string &out) {
  serialize_fields(entries, out);

  if (checked.chunked) {
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

  if (checked.add_length) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  out += "\r\n";
  out += body;
}

} // namespace

namespace {

// What serialize() and a streamed head ask of a request's start line, and what its method allows.
[[nodiscard]] framing_rules request_rules(request const &message) {
  check_target(message.target);

  auto const method_has_body =
      message.method == verb::post || message.method == verb::put || message.method == verb::patch;

  framing_rules rules;

  rules.add_length = !message.body.empty() || method_has_body;
  rules.chunked_allowed = message.version != version::http_1_0;
  // What follows the head of a CONNECT is the tunnel, as a parser reads it: a body would reach the far end.
  rules.body_allowed = message.method != verb::connect;
  return rules;
}

void append_start_line(request const &message, std::string &out) {
  out += to_string(message.method);
  out += ' ';
  out += message.target;
  out += ' ';
  out += to_string(message.version);
  out += "\r\n";
}

} // namespace

void serialize(request const &message, std::string &out) {
  auto const rules = request_rules(message);

  // Only what streams a request can write it.
  if (message.body_writer) {
    reject("a request with a body_writer goes in pieces");
  }

  auto const checked = check_framing(message.fields, message.body, message.trailers, rules);

  append_start_line(message, out);
  serialize_framing(message.fields, message.body, message.trailers, checked, out);
}

namespace {

// What serialize() and a streamed head ask of a response's start line, and what its status allows.
[[nodiscard]] framing_rules response_rules(response const &message) {
  auto const code = static_cast<std::uint16_t>(message.status);

  if (code < 100 || code > 999) {
    throw std::invalid_argument{"quoll::http: invalid status code"};
  }

  if (has_line_break(message.reason)) {
    throw std::invalid_argument{"quoll::http: invalid reason phrase"};
  }

  // RFC 9110: 1xx, 204 and 304 responses never have a body.
  auto const has_body = !is_informational(message.status) && message.status != status::no_content &&
                        message.status != status::not_modified;

  framing_rules rules;

  rules.body_allowed = has_body;
  rules.length_allowed = !is_informational(message.status) && message.status != status::no_content;
  rules.add_length = has_body;
  rules.chunked_allowed = message.version != version::http_1_0;
  rules.length_without_body = true;
  return rules;
}

void append_start_line(response const &message, std::string &out) {
  out += to_string(message.version);
  out += ' ';
  out += std::to_string(static_cast<std::uint16_t>(message.status));
  out += ' ';
  out += message.reason.empty() ? reason_phrase(message.status) : std::string_view{message.reason};
  out += "\r\n";
}

} // namespace

void serialize(response const &message, std::string &out) {
  auto const rules = response_rules(message);

  // Only what streams a response can write it.
  if (message.body_writer) {
    reject("a response with a body_writer goes in pieces");
  }

  auto const checked = check_framing(message.fields, message.body, message.trailers, rules);

  append_start_line(message, out);
  serialize_framing(message.fields, message.body, message.trailers, checked, out);
}

namespace detail {

streamed_framing serialize_streamed_head(response const &message, std::string &out) {
  auto rules = response_rules(message);

  if (!rules.body_allowed || !message.body.empty()) {
    reject("a body_writer on a response that cannot have one, or with a body");
  }

  rules.add_length = false;

  auto const checked = check_framing(message.fields, message.body, message.trailers, rules);
  auto const length = declared_length(message.fields);

  append_start_line(message, out);
  serialize_fields(message.fields, out);
  out += "\r\n";

  return {.chunked = checked.chunked, .length = length ? std::optional<std::uint64_t>{*length} : std::nullopt};
}

std::optional<std::uint64_t> announced_length(fields const &entries) {
  auto const length = declared_length(entries);

  return length ? std::optional<std::uint64_t>{*length} : std::nullopt;
}

streamed_framing serialize_streamed_head(request const &message, std::string &out) {
  auto rules = request_rules(message);

  if (!rules.body_allowed || !message.body.empty()) {
    reject("a body_writer on a request that cannot have one, or with a body");
  }

  rules.add_length = false;
  rules.length_without_body = true;

  // Chunks unless it says otherwise, which is for this to say rather than for the caller, as for what sets its fields.
  auto const announced = declared_length(message.fields) || last_coding_is_chunked(message.fields);
  auto with_chunks = message.fields;

  if (!announced) {
    with_chunks.add("Transfer-Encoding", "chunked");
  }

  auto const checked = check_framing(with_chunks, message.body, message.trailers, rules);
  auto const length = declared_length(with_chunks);

  append_start_line(message, out);
  serialize_fields(with_chunks, out);
  out += "\r\n";

  return {.chunked = checked.chunked, .length = length ? std::optional<std::uint64_t>{*length} : std::nullopt};
}

streamed_wire::streamed_wire(std::string head, streamed_framing framing) noexcept
    : head_(std::move(head)), framing_(framing), left_(framing.length.value_or(0)) {}

bool streamed_wire::past_its_length(std::string_view piece) const noexcept {
  return framing_.length && piece.size() > left_;
}

bool streamed_wire::started() const noexcept { return started_; }

bool streamed_wire::short_of_its_length() const noexcept { return left_ != 0; }

void streamed_wire::append_head() {
  if (!started_) {
    started_ = true;
    wire_ += head_;
  }
}

std::string_view streamed_wire::frame(std::string_view piece) {
  if (framing_.length) {
    left_ -= piece.size();
  }

  if (started_ && (!framing_.chunked || piece.empty())) {
    return piece;
  }

  wire_.clear();
  append_head();

  if (framing_.chunked) {
    append_chunk(piece, wire_);
  } else {
    wire_ += piece;
  }

  return wire_;
}

std::string_view streamed_wire::end(fields const &trailers) {
  wire_.clear();
  append_head();

  if (framing_.chunked) {
    append_last_chunk(trailers, wire_);
  }

  return wire_;
}

void append_chunk(std::string_view piece, std::string &out) {
  if (piece.empty()) {
    return;
  }

  append_hex(piece.size(), out);
  out += "\r\n";
  out += piece;
  out += "\r\n";
}

void append_last_chunk(fields const &trailers, std::string &out) {
  out += "0\r\n";
  serialize_fields(trailers, out);
  out += "\r\n";
}

} // namespace detail

std::string serialize(request const &message) {
  std::string out;

  serialize(message, out);
  return out;
}

std::string serialize(response const &message) {
  std::string out;

  serialize(message, out);
  return out;
}

} // namespace quoll::http
