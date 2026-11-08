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

#ifndef QUOLL_REST_EVENT_STREAM_HPP
#define QUOLL_REST_EVENT_STREAM_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/detail/client_response.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace quoll::rest {

/**
 * @brief The events of an endpoint with events<T, EventCodec>, as a client's `subscribe` gives them, each decoded.
 *
 * @code
 * auto changes = co_await client.subscribe<feed>(std::nullopt);
 *
 * while (auto const change = co_await changes.next()) {
 *   show(*change);
 * }
 * @endcode
 *
 * next() gives nothing once the stream has ended; last_event_id() is then what to resume from, if not empty (a stream
 * that ended before an event with an id leaves the position the caller had), an argument of the
 * endpoint (a `header<"Last-Event-ID", std::optional<std::string>>` say). An event's type is not handed over: one
 * endpoint, one type, every event decoded as `T`, a heartbeat of a third-party API's own type included, which then
 * throws as an event that does not decode. Reading fails, and ends, as http::event_reader says; an event that does not
 * decode throws call_error, holding it as the body of its response, and reading goes on.
 *
 * Moved, not copied. Thread safety: as http::event_reader.
 */
template <class T, class EventCodec = json_codec>
class event_stream {
public:
  /** @brief Reads `events`; a client's `subscribe` makes one. */
  explicit event_stream(http::event_reader events) noexcept : events_(std::move(events)) {}

  /**
   * @brief The next value, or nothing once the stream has ended.
   *
   * @throws call_error, from the task, for an event that does not decode; as http::event_reader::next() otherwise.
   */
  [[nodiscard]] async::task<std::optional<T>> next() { return decoded(events_.next()); }

  /** @brief The id of the last complete event, empty if none: what to resume from. */
  [[nodiscard]] std::string last_event_id() const { return events_.last_event_id(); }

  /** @brief How long the stream asked to wait before connecting again, if it did. */
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept { return events_.retry(); }

private:
  // No `this`: the reader's task holds what it reads.
  [[nodiscard]] static async::task<std::optional<T>> decoded(async::task<std::optional<http::server_sent_event>> next) {
    auto event = co_await std::move(next);

    if (!event) {
      co_return std::nullopt;
    }

    auto value = EventCodec{}.template decode<T>(event->data);

    if (!value) {
      http::response received;

      received.status = http::status::ok;
      received.body = std::move(event->data);

      detail::fail(unexpected_response{std::move(received), "invalid event: " + value.error().message});
    }

    co_return std::move(*value);
  }

  http::event_reader events_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_EVENT_STREAM_HPP
