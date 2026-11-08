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

#include <quoll/http/detail/number.hpp>
#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/event_parser.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::http {

namespace {

constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

// A `retry` value: ASCII digits only, or nothing. A long one is cut to the most a reader waits, the digits of one too
// long for a number included.
[[nodiscard]] std::optional<std::chrono::milliseconds> retry_value(std::string_view value) noexcept {
  if (value.empty() ||
      !std::ranges::all_of(value, [](char character) { return character >= '0' && character <= '9'; })) {
    return std::nullopt;
  }

  auto const count = detail::parse_number<std::int64_t>(value);

  if (!count || *count > event_parser::max_retry.count()) {
    return event_parser::max_retry;
  }

  return std::chrono::milliseconds{*count};
}

// `value` appended to `out` as text, what is not UTF-8 read as U+FFFD.
void append_text(std::string &out, std::string_view value) {
  if (detail::is_utf8(value)) {
    out.append(value);
  } else {
    detail::append_repaired_utf8(out, value);
  }
}

} // namespace

void event_parser::feed(std::string_view bytes) {
  // Nothing would read them any more: held, they would only grow.
  if (failed_) {
    throw std::system_error{error::event_too_large, "quoll::http::event_parser"};
  }

  // Events not taken yet: one piece after another, they would only grow too.
  if (!drained_) {
    throw std::logic_error{"quoll::http::event_parser: fed before next() gave every event"};
  }

  buffer_.append(bytes);
  drained_ = false;
}

std::optional<server_sent_event> event_parser::next() {
  if (failed_) {
    throw std::system_error{error::event_too_large, "quoll::http::event_parser"};
  }

  if (!started_) {
    auto const start = std::string_view{buffer_}.substr(position_);

    // A BOM split between two pieces: wait for the rest of it.
    if (start.size() < byte_order_mark.size() && byte_order_mark.starts_with(start)) {
      drained_ = true;
      return std::nullopt;
    }

    if (start.starts_with(byte_order_mark)) {
      position_ += byte_order_mark.size();
    }

    started_ = true;
  }

  for (;;) {
    if (after_cr_ && position_ < buffer_.size()) {
      if (buffer_[position_] == '\n') {
        ++position_;
      }

      after_cr_ = false;
    }

    auto const rest = std::string_view{buffer_}.substr(position_);
    auto const end = rest.find_first_of("\r\n", scanned_);

    if (end == std::string_view::npos) {
      // A line fed a byte at a time is searched once, not once a byte.
      buffer_.erase(0, position_);
      position_ = 0;
      scanned_ = buffer_.size();
      check_size(buffer_.size() + data_.size() + type_.size());
      drained_ = true;
      return std::nullopt;
    }

    scanned_ = 0;

    auto const line = rest.substr(0, end);

    // As it would have been while it came, so that how the body is cut changes nothing.
    check_size(line.size() + data_.size() + type_.size());
    position_ += end + 1;
    after_cr_ = rest[end] == '\r';

    if (auto event = take_line(line)) {
      return event;
    }
  }
}

std::optional<server_sent_event> event_parser::take_line(std::string_view line) {
  if (line.empty()) {
    return dispatch();
  }

  if (line.front() == ':') {
    return std::nullopt;
  }

  auto const colon = line.find(':');
  auto const field = line.substr(0, colon);
  auto value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);

  if (value.starts_with(' ')) {
    value.remove_prefix(1);
  }

  // What is not UTF-8 is repaired in the values kept only: no field's name has such bytes, and the ASCII of a colon or
  // a space is never part of what is replaced.
  if (field == "event") {
    type_.clear();
    append_text(type_, value);
  } else if (field == "data") {
    append_text(data_, value);
    data_.push_back('\n');
  } else if (field == "id") {
    if (!value.contains('\0')) {
      id_buffer_.clear();
      append_text(id_buffer_, value);
    }
  } else if (field == "retry") {
    if (auto const delay = retry_value(value)) {
      retry_ = delay;
    }
  }

  check_size(data_.size() + type_.size());
  return std::nullopt;
}

std::optional<server_sent_event> event_parser::dispatch() {
  // Even for a block with no data, as for a browser. Copied only when it changed.
  if (last_event_id_ != id_buffer_) {
    last_event_id_ = id_buffer_;
  }

  if (data_.empty()) {
    type_.clear();
    return std::nullopt;
  }

  data_.pop_back();

  server_sent_event event;

  event.data = std::exchange(data_, {});

  if (!type_.empty()) {
    event.type = std::exchange(type_, {});
  }

  if (!last_event_id_.empty()) {
    event.id = last_event_id_;
  }

  return event;
}

void event_parser::check_size(std::size_t held) {
  if (held > max_event_size_) {
    failed_ = true;
    throw std::system_error{error::event_too_large, "quoll::http::event_parser"};
  }
}

} // namespace quoll::http
