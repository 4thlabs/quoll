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

#ifndef QUOLL_HTTP_EVENT_READER_HPP
#define QUOLL_HTTP_EVENT_READER_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/event_parser.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace quoll::http {

/**
 * @brief Reads the events of a `text/event-stream` response as they come, natively and through a browser's `fetch`.
 *
 * @code
 * auto response = co_await client.stream(request);
 *
 * if (response.status != http::status::ok ||
 *     !response.fields.find("Content-Type").value_or("").starts_with("text/event-stream")) {
 *   co_return;
 * }
 *
 * http::event_reader events{response.body};
 *
 * while (auto const event = co_await events.next()) {
 *   show(event->type, event->data);
 * }
 * @endcode
 *
 * The status and the `Content-Type` are for the caller to check, which knows what else it accepts. When the stream
 * ends, next() gives nothing, and last_event_id() and retry() say how to ask for the rest: it does not connect again
 * on its own. A read that fails, or an event larger than it holds, throws from next() (see response_body and
 * event_parser), and reading ends there.
 *
 * Moved, not copied: copies would share the body, not the events read from it. What it reads lives with the task of
 * next() too, so that the reader may be moved, or go, while one is pending. Thread safety: one next() at a time.
 */
class event_reader {
public:
  /** @brief Reads `body`, an event holding at most `max_event_size` bytes (see event_parser). */
  explicit event_reader(response_body body, std::size_t max_event_size = event_parser::default_max_event_size);

  event_reader(event_reader const &) = delete;
  event_reader &operator=(event_reader const &) = delete;
  event_reader(event_reader &&) noexcept = default;
  event_reader &operator=(event_reader &&) noexcept = default;
  ~event_reader() = default;

  /**
   * @brief The next event, or nothing once the stream has ended, or on a reader moved from.
   *
   * @throws std::logic_error, from the task, while another next() is pending.
   */
  [[nodiscard]] async::task<std::optional<server_sent_event>> next();

  /** @brief The ID of the last event the stream ended, empty if none: `Last-Event-ID`, to connect again. A copy. */
  [[nodiscard]] std::string last_event_id() const;

  /** @brief How long the stream asked to wait before connecting again, if it did. */
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept;

private:
  struct state {
    response_body body;
    event_parser parser;
    // Atomic: a second next() from another thread is misuse it reports, not a race of its own.
    std::atomic<bool> reading = false;
  };

  [[nodiscard]] static async::task<std::optional<server_sent_event>> read_next(std::shared_ptr<state> reading);

  // Null once moved from.
  std::shared_ptr<state> state_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_EVENT_READER_HPP
