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

// A text/event-stream as a server sends it: however the body is cut, the same events come, and each one written again
// reads back as itself.

#include <quoll/fuzz/support.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/event_parser.hpp>
#include <quoll/http/format_event.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace http = quoll::http;

namespace {

// Low, so that inputs reach it.
constexpr std::size_t max_event_size = 256;

struct outcome {
  std::vector<http::server_sent_event> events;
  bool too_large = false;
  std::string last_event_id;
  std::optional<std::chrono::milliseconds> retry;

  bool operator==(outcome const &) const = default;
};

[[nodiscard]] outcome read_all(std::string_view input, std::size_t piece) {
  http::event_parser parser{max_event_size};
  outcome result;

  try {
    for (std::size_t position = 0; position < input.size(); position += piece) {
      parser.feed(input.substr(position, piece));

      while (auto event = parser.next()) {
        result.events.push_back(std::move(*event));
      }
    }
  } catch (std::system_error const &failure) {
    quoll::fuzz::require(failure.code() == http::error::event_too_large);
    result.too_large = true;
  }

  result.last_event_id = parser.last_event_id();
  result.retry = parser.retry();

  return result;
}

// The event behind a comment and the stream's retry, which a reader skips and takes, written again.
void check_written_again(http::server_sent_event const &event, outcome const &read) {
  http::event_parser parser{max_event_size * 8};

  parser.feed(http::format_comment(event.type) + (read.retry ? http::format_retry(*read.retry) : std::string{}) +
              http::format_event(event));

  auto const again = parser.next();

  quoll::fuzz::require(again == event);
  quoll::fuzz::require(!parser.next());
  quoll::fuzz::require(parser.retry() == read.retry);
  quoll::fuzz::require(parser.last_event_id() == event.id.value_or(""));
}

} // namespace

// Every event written again in order, into one parser: the same events, the id carried from one to the next and its
// resets included.
void check_stream_written_again(outcome const &read) {
  std::string text;

  for (auto const &event : read.events) {
    text += http::format_event(event);
  }

  http::event_parser parser{max_event_size * 8};
  std::vector<http::server_sent_event> again;

  parser.feed(text);

  while (auto event = parser.next()) {
    again.push_back(std::move(*event));
  }

  quoll::fuzz::require(again == read.events);
}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // The first byte says in what pieces the bytes come. 0: all at once.
  auto const piece = data[0] == 0 ? size : std::size_t{data[0]};
  auto const input = quoll::fuzz::as_text(data + 1, size - 1);
  auto const whole = read_all(input, input.size() + 1);

  quoll::fuzz::require(read_all(input, piece) == whole);

  for (auto const &event : whole.events) {
    check_written_again(event, whole);
  }

  check_stream_written_again(whole);

  return 0;
}
