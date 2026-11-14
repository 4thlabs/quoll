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
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/detail/client_response.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
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
 * decode throws an http::http_error received, holding it as the body of its response, and reading goes on. A `T` that
 * holds views into its event (a `std::string_view` Glaze reads) is valid up to the next next(), for as long as the
 * stream.
 *
 * Moved, not copied: one moved from throws std::logic_error from next(). One next() at a time, its decoding included:
 * one asked while another is pending throws std::logic_error. Thread safety: as http::event_reader.
 */
template <class T, class EventCodec = json_codec>
class event_stream {
public:
  /** @brief Reads `events`; a client's `subscribe` makes one. */
  explicit event_stream(http::event_reader events) : events_(std::move(events)) {}

  /**
   * @brief The next value, or nothing once the stream has ended.
   *
   * @throws an http::http_error received, from the task, for an event that does not decode; as
   * http::event_reader::next() otherwise.
   */
  [[nodiscard]] async::task<std::optional<T>> next() {
    if (!state_) {
      throw std::logic_error{"quoll::rest::event_stream: next() once moved from"};
    }

    // Its turn, for the whole of it: the read and the decoding, which the text kept here outlives.
    http::detail::one_at_a_time turn{
        std::shared_ptr<std::atomic<bool>>{state_, &state_->busy},
        "quoll::rest::event_stream: next() while another is pending"
    };

    return decoded(std::move(turn), events_.next(), state_);
  }

  /** @brief The id of the last complete event, empty if none: what to resume from. */
  [[nodiscard]] std::string last_event_id() const { return events_.last_event_id(); }

  /** @brief How long the stream asked to wait before connecting again, if it did. */
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept { return events_.retry(); }

private:
  // What a stream keeps: the text of the last event, until the next, which a value decoded into views of it points
  // into; and the turn of one next() at a time, held until its value is made.
  struct state {
    std::string data;
    std::atomic<bool> busy = false;
  };

  // No `this`: the reader's task holds what it reads, and `kept` the event's text, which a `T` may point into.
  [[nodiscard]] static async::task<std::optional<T>> decoded(
      http::detail::one_at_a_time turn, async::task<std::optional<http::server_sent_event>> next,
      std::shared_ptr<state> kept
  ) {
    auto const held = std::move(turn);
    auto *const data = &kept->data;
    auto event = co_await std::move(next);

    if (!event) {
      co_return std::nullopt;
    }

    *data = std::move(event->data);

    auto value = EventCodec{}.template decode<T>(*data);

    if (!value) {
      http::response received;

      received.status = http::status::ok;
      received.body = *data;

      detail::fail(unexpected_response{std::move(received), "invalid event: " + value.error().message});
    }

    co_return std::move(*value);
  }

  http::event_reader events_;
  std::shared_ptr<state> state_ = std::make_shared<state>();
};

} // namespace quoll::rest

#endif // QUOLL_REST_EVENT_STREAM_HPP
