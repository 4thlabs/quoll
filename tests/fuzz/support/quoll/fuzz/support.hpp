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

#ifndef QUOLL_FUZZ_SUPPORT_HPP
#define QUOLL_FUZZ_SUPPORT_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/pollable_stream.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::fuzz {

// libFuzzer's inputs stay under 4 KB: with the default limits, what a parser does past them would never run.
inline constexpr http::parse_limits low_limits{
    .max_header_size = 1024, .max_field_count = 16, .max_body_size = 1024, .max_streamed_body_size = 2048
};

// What must hold whatever the input: a libFuzzer crash, reported with the input that broke it.
inline void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

[[nodiscard]] inline std::string_view as_text(std::uint8_t const *data, std::size_t size) noexcept {
  return {reinterpret_cast<char const *>(data), size};
}

// A message's fields as they are, but for Content-Length, which serializing may add.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> without_length(http::fields const &fields) {
  std::vector<std::pair<std::string, std::string>> kept;

  for (auto const &field : fields) {
    if (!http::detail::iequals(field.name, "Content-Length")) {
      kept.emplace_back(field.name, field.value);
    }
  }

  return kept;
}

// Feeds `input` in pieces of `piece` bytes, as a socket cuts them anywhere, then ends the stream: `on_message` gets
// each complete message, including one only the end of the stream completes.
template <class Parser, class OnMessage>
void feed_in_pieces(Parser &parser, std::string_view input, std::size_t piece, OnMessage on_message) {
  while (!input.empty()) {
    auto const fed = input.substr(0, piece);
    auto const result = parser.feed(std::span{fed.data(), fed.size()});

    require(result.consumed <= fed.size());
    input.remove_prefix(result.consumed);

    if (result.error) {
      return;
    }

    if (parser.done()) {
      on_message(parser.release());
    } else if (result.consumed == 0) {
      return;
    }
  }

  if (!parser.finish() && parser.done()) {
    on_message(parser.release());
  }
}

// A body in pieces is the body of the same bytes read whole, when the limits let both through. `prepare` readies a
// parser.
template <class Parser, class Prepare>
void check_body_in_pieces(std::string_view input, std::size_t piece, Prepare prepare) {
  Parser whole{low_limits};
  Parser streamed{low_limits};
  std::string held;
  std::string taken;
  auto left = input;

  prepare(whole);
  prepare(streamed);
  streamed.set_body_streaming([](auto const &) { return true; });

  while (!left.empty() && !streamed.done()) {
    auto const fed = left.substr(0, piece);
    auto const result = streamed.feed(std::span{fed.data(), fed.size()});

    streamed.take_body(held);
    taken += held;
    left.remove_prefix(result.consumed);

    if (result.error || result.consumed == 0) {
      break;
    }
  }

  if (!streamed.done() && streamed.finish()) {
    return;
  }

  streamed.take_body(held);
  taken += held;

  auto const all = input.substr(0, input.size() - left.size());
  auto const result = whole.feed(std::span{all.data(), all.size()});

  if (!result.error && !whole.done()) {
    static_cast<void>(whole.finish());
  }

  if (whole.done() && streamed.done()) {
    require(whole.release().body == taken);
  }

  if (!streamed.done()) {
    return;
  }

  // What the first message counted goes with it: the rest of the input is another one, read as they all are.
  static_cast<void>(streamed.release());
  require(!streamed.body_streamed());
  feed_in_pieces(streamed, left, piece, [](auto const &) {});
}

// A message as bytes, nothing when the serializer refuses it: a parser lets through more than it may send.
template <class Message>
[[nodiscard]] std::optional<std::string> serialized(Message const &message) {
  try {
    return http::serialize(message);
  } catch (std::invalid_argument const &) {
    return std::nullopt;
  }
}

// Reads back what a message serializes to, nothing when it does not: all of the bytes, as what is left would be another
// message, with the same version, fields, trailers and body. The caller compares the start line.
template <class Message>
[[nodiscard]] std::optional<Message> read_back(Message const &message) {
  auto const wire = serialized(message);

  if (!wire) {
    return std::nullopt;
  }

  // Serializing may add a field, Content-Length, so that a message at a limit leaves past it: not what is checked here.
  http::parser<Message> again{
      {.max_header_size = std::numeric_limits<std::size_t>::max(),
       .max_field_count = std::numeric_limits<std::size_t>::max()}
  };
  auto const reparsed = again.feed(std::span{wire->data(), wire->size()});

  require(!reparsed.error);
  require(again.done());
  require(reparsed.consumed == wire->size());

  auto same = again.release();

  require(same.body == message.body);
  require(same.version == message.version);
  require(without_length(same.fields) == without_length(message.fields));
  require(without_length(same.trailers) == without_length(message.trailers));
  return same;
}

// A stream whose peer sends `input` in pieces of `chunk` bytes, or at once for 0, then closes; what is written to it is
// dropped. Nothing suspends, so that a task runs to its end on the input alone, and a server cannot wait for more.
// Hands out the start of `input`, as much as `buffer` holds and at most `limit` bytes, as a stream's read gives it.
[[nodiscard]] inline std::size_t
take_input(std::string_view &input, std::span<std::byte> buffer, std::size_t limit) noexcept {
  auto const count = std::min({buffer.size(), input.size(), limit});

  std::memcpy(buffer.data(), input.data(), count);
  input.remove_prefix(count);
  return count;
}

class input_stream {
public:
  struct awaitable {
    io::io_result result;

    [[nodiscard]] bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>, async::await_context const &) const noexcept {}

    [[nodiscard]] io::io_result await_resume() const noexcept { return result; }
  };

  input_stream(std::string_view input, std::size_t chunk) noexcept
      : input_(input), chunk_(chunk == 0 ? input.size() : chunk) {}

  [[nodiscard]] awaitable read_some(std::span<std::byte> buffer) noexcept {
    if (input_.empty()) {
      return {{io::error::end_of_stream, 0}};
    }

    return {{{}, take_input(input_, buffer, chunk_)}};
  }

  [[nodiscard]] awaitable write_some(std::span<std::byte const> data) const noexcept { return {{{}, data.size()}}; }

  [[nodiscard]] awaitable shutdown_send() const noexcept { return {{}}; }

  // A peer that keeps quiet as the connection idles while input is left, so that what reuses one, a POST included,
  // runs on it, and is gone at its end, as its next read says, so that what takes it drops it.
  [[nodiscard]] io::read_readiness poll_read() const noexcept {
    return input_.empty() ? io::read_readiness::ready : io::read_readiness::would_wait;
  }

private:
  std::string_view input_;
  std::size_t chunk_;
};

static_assert(io::HalfClosableStream<input_stream>);
static_assert(io::PollableStream<input_stream>);

} // namespace quoll::fuzz

#endif // QUOLL_FUZZ_SUPPORT_HPP
