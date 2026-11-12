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

#ifndef QUOLL_REST_EVENT_SINK_HPP
#define QUOLL_REST_EVENT_SINK_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/event_writer.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/server_sent_event.hpp>
#include <quoll/rest/event_options.hpp>
#include <quoll/rest/json_codec.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest {

namespace detail {

struct event_sink_access;

// What a sink and its operations share, so that an operation never reaches a sink gone: the response_writer until the
// stream starts, for an error to answer through, then the event_writer; and the turn of one operation at a time.
struct event_sink_state {
  std::optional<http::response_writer> out;
  std::optional<http::event_writer> events;
  std::atomic<bool> busy = false;
  // The operations running, which the stream waits for before it ends, and its top bit, set once the service has
  // ended: one atomic word, so that an operation starts only while the sink is open, and closing sees it.
  std::atomic<std::uint32_t> activity = 0;
};

// The turn of an operation, taken when it is asked, so that operations run in that order (see http::event_writer).
[[nodiscard]] http::detail::one_at_a_time sink_turn(std::shared_ptr<event_sink_state> const &state);

// The operations, run in their turn: the writer is made, and the stream started, when the first one runs. What one
// refuses is refused before the stream starts, rather than once its writer has taken the response.
[[nodiscard]] async::task<> sink_open(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn);
[[nodiscard]] async::task<>
sink_send(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn, http::server_sent_event event);
[[nodiscard]] async::task<>
sink_comment(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn, std::string text);
[[nodiscard]] async::task<>
sink_retry(std::shared_ptr<event_sink_state> state, http::detail::one_at_a_time turn, std::chrono::milliseconds delay);

// The answer to an http_error thrown before the stream started.
[[nodiscard]] async::task<> sink_answer(std::shared_ptr<event_sink_state> state, http::response refusal);

// Once drained: the head of a stream with no event, unless one has started it, then the sink closed.
[[nodiscard]] async::task<> sink_finish(std::shared_ptr<event_sink_state> state);

// Once the service has ended: the sink closed to operations not running yet, which then throw, and those running, on
// tasks of the service's own, ended.
[[nodiscard]] async::task<> sink_drain(std::shared_ptr<event_sink_state> state);

// Closes the sink: what writes to the connection goes now, and an operation run later throws.
void sink_close(event_sink_state &state) noexcept;

// Closes a sink when it goes, on every way out of the stream.
class sink_closing {
public:
  explicit sink_closing(std::shared_ptr<event_sink_state> state) noexcept : state_(std::move(state)) {}

  sink_closing(sink_closing const &) = delete;
  sink_closing &operator=(sink_closing const &) = delete;
  sink_closing(sink_closing &&) = delete;
  sink_closing &operator=(sink_closing &&) = delete;

  ~sink_closing() { sink_close(*state_); }

private:
  std::shared_ptr<event_sink_state> state_;
};

} // namespace detail

/**
 * @brief What the service of an endpoint with events<T, EventCodec> sends its events through, given as its last
 * argument, by reference.
 *
 * @code
 * async::task<> operator()(feed, std::optional<std::string> last_id, rest::event_sink<change> &events) {
 *   check_feed(last_id);              // an http_error here still answers: the stream has not started
 *   co_await events.open();           // the head, so that a client sees the stream before any change
 *
 *   for (;;) {
 *     auto const next = co_await changes.after(last_id);   // with a timeout and comments, see http::event_writer
 *
 *     co_await events.send(next, {.id = std::to_string(next.id)});
 *     last_id = std::to_string(next.id);
 *   }
 * }
 * @endcode
 *
 * The head leaves with open() or the first event, comment or retry, once it runs, as with http::event_writer, which
 * it writes through. Until then, an http_error the service throws answers as from any service, a declared error
 * response say; after, it cuts the stream: what may refuse the request comes before open(). Keep-alive comments come
 * from the loop that sends, as http::event_writer explains. One operation at a time: one asked while another is pending
 * throws std::logic_error.
 *
 * It lives as long as the service's task, which mount() awaits: neither stored nor moved. Its operations are awaited
 * by the service, on its task or one of its own: when it ends, the stream waits for those running, and one asked
 * before and not running yet, kept or spawned past it, throws std::logic_error when it runs. The sink itself goes with
 * the service: a reference to it kept past it dangles, as any to a local does. A service that ends with no
 * event still answers a stream, with none. Thread safety: none, as it makes its writer with its first operation: from
 * the service's task, or one task at a time.
 */
template <class T, class EventCodec = json_codec>
class event_sink {
public:
  /** @brief Writes through `out`; mount() makes one for the service. */
  explicit event_sink(http::response_writer &&out) : state_(std::make_shared<detail::event_sink_state>()) {
    state_->out.emplace(std::move(out));
  }

  event_sink(event_sink const &) = delete;
  event_sink &operator=(event_sink const &) = delete;
  event_sink(event_sink &&) = delete;
  event_sink &operator=(event_sink &&) = delete;
  ~event_sink() = default;

  /** @brief Sends the head now, so that a client sees the stream before the first event. */
  [[nodiscard]] async::task<> open() { return detail::sink_open(state_, detail::sink_turn(state_)); }

  /**
   * @brief Sends `value`, encoded by `EventCodec`, as an event.
   *
   * @throws what the codec throws, at the call; from the task, what http::format_event throws for the event, before
   * anything leaves: the stream has not started then, and an http_error after it still answers.
   */
  [[nodiscard]] async::task<> send(T const &value, event_options options = {}) {
    http::server_sent_event event{
        .type = std::move(options.type), .data = EventCodec{}.encode(value), .id = std::move(options.id)
    };

    return detail::sink_send(state_, detail::sink_turn(state_), std::move(event));
  }

  /** @brief A comment, which a client skips: what keeps a quiet stream alive (see http::event_writer). */
  [[nodiscard]] async::task<> comment(std::string_view text = {}) {
    return detail::sink_comment(state_, detail::sink_turn(state_), std::string{text});
  }

  /** @brief How long a client waits before it connects again once the stream has ended. */
  [[nodiscard]] async::task<> retry(std::chrono::milliseconds delay) {
    return detail::sink_retry(state_, detail::sink_turn(state_), delay);
  }

  /**
   * @brief Whether the stream has started: an operation has run, and an http_error then cuts it rather than answer.
   * One asked and not run yet, or dropped, has not started it.
   */
  [[nodiscard]] bool started() const noexcept { return state_->events.has_value(); }

private:
  friend struct detail::event_sink_access;

  std::shared_ptr<detail::event_sink_state> state_;
};

namespace detail {

// What mount() reaches in a sink.
struct event_sink_access {
  template <class T, class EventCodec>
  [[nodiscard]] static async::task<> answer(event_sink<T, EventCodec> const &sink, http::response refusal) {
    return sink_answer(sink.state_, std::move(refusal));
  }

  template <class T, class EventCodec>
  [[nodiscard]] static async::task<> finish(event_sink<T, EventCodec> const &sink) {
    return sink_finish(sink.state_);
  }

  template <class T, class EventCodec>
  [[nodiscard]] static async::task<> drain(event_sink<T, EventCodec> const &sink) {
    return sink_drain(sink.state_);
  }

  template <class T, class EventCodec>
  [[nodiscard]] static sink_closing closing(event_sink<T, EventCodec> const &sink) noexcept {
    return sink_closing{sink.state_};
  }
};

} // namespace detail

} // namespace quoll::rest

#endif // QUOLL_REST_EVENT_SINK_HPP
