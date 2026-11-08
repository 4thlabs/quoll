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

#ifndef QUOLL_HTTP_EVENT_WRITER_HPP
#define QUOLL_HTTP_EVENT_WRITER_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace quoll::http {

/**
 * @brief Writes a `text/event-stream` response, an event at a time, through the response_writer of a handler.
 *
 * @code
 * router.get("/feed", [](http::response_writer out) -> async::task<> {
 *   http::event_writer events{std::move(out)};
 *
 *   co_await events.open();
 *
 *   for (auto const &change : changes) {
 *     co_await events.send({.type = "change", .data = to_json(change), .id = change.id});
 *   }
 * });
 * @endcode
 *
 * It sets `Content-Type: text/event-stream` and `X-Accel-Buffering: no`, unless the handler set them, a middleware's
 * replaced as a handler's field replaces one, as a stream needs them, which tells a
 * proxy such as nginx to pass each event on as it comes rather than hold them. A `Cache-Control` is left to the handler
 * or a middleware: set here, it would replace a middleware's. Status and other fields are set on the response_writer
 * before it is given, with nothing written through it yet. The head leaves with open(), or the first event: open() at
 * once lets a client see the stream before there is anything to say.
 *
 * Stopping a server waits for a stream as for any request under way, up to its `shutdown_timeout`, then stops what the
 * handler awaits: a stream that waits for changes ends then, not before.
 *
 * A client's `response_timeout` applies to each piece it waits for: a stream with nothing to say for a while sends a
 * comment() more often than that, every 15 seconds say, which also keeps a proxy from closing it. An event goes when
 * send() completes, as with response_writer::write(), and a peer gone makes it throw, which ends the handler.
 *
 * Moved, not copied, as the response_writer it holds: one moved from throws std::logic_error when asked anything. An
 * operation takes what it writes through when it is called, so that the writer may move, or go, before it ends, within
 * the handler. Like the response_writer, it does not outlive the handler's task: what it writes through is the
 * connection's, which goes with the response. A stream that others feed (a hub of subscribers) is a handler that waits
 * for what they post, and writes it itself.
 * One operation at a time, a comment to keep the stream alive included: one asked for while another is pending throws
 * std::logic_error when it is asked, in every build, so that events leave in the order they were asked for. Thread
 * safety: operations from tasks on other threads are reported so, not raced, as the flag is atomic; what they write
 * through is the connection's, one write at a time.
 */
class event_writer {
public:
  /**
   * @brief Writes through `out`, whose fields it completes.
   *
   * @throws std::logic_error for a response_writer whose head is given, or moved from: its fields would do nothing.
   */
  explicit event_writer(response_writer out);

  event_writer(event_writer const &) = delete;
  event_writer &operator=(event_writer const &) = delete;
  event_writer(event_writer &&) noexcept = default;
  event_writer &operator=(event_writer &&) noexcept = default;
  ~event_writer() = default;

  /** @brief Sends the head now, before any event. */
  [[nodiscard]] async::task<> open();

  /**
   * @brief Sends `event` (see http::format_event).
   *
   * @throws std::invalid_argument, before the task runs, for an event format_event refuses.
   */
  [[nodiscard]] async::task<> send(server_sent_event const &event);

  /**
   * @brief Asks the client to wait `delay` before connecting again once the stream has ended (see format_retry).
   *
   * @throws std::invalid_argument, before the task runs, for a negative delay.
   */
  [[nodiscard]] async::task<> retry(std::chrono::milliseconds delay);

  /**
   * @brief A comment, which a client ignores: what keeps a quiet stream alive (see format_comment).
   *
   * @throws std::invalid_argument, before the task runs, for text that is not UTF-8 or holds a line break.
   */
  [[nodiscard]] async::task<> comment(std::string_view text = {});

private:
  // What each operation's task holds too, from the call on, so that the writer may move, or go, before it ends.
  struct state {
    response_writer out;
    // Up while an operation is pending, so that a second one throws rather than write over the first.
    std::atomic<bool> busy = false;
  };

  // The flag of one operation, taken when it is asked, in its order, and let down when its task ends, run or not.
  class operation;

  // The state, or std::logic_error for a writer moved from.
  [[nodiscard]] std::shared_ptr<state> checked() const;

  // `text` lives in the frame until it has left.
  [[nodiscard]] static async::task<> write(operation writing, std::string text);

  // Null once moved from.
  std::shared_ptr<state> state_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_EVENT_WRITER_HPP
