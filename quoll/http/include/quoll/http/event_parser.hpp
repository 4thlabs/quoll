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

#ifndef QUOLL_HTTP_EVENT_PARSER_HPP
#define QUOLL_HTTP_EVENT_PARSER_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::http {

/**
 * @brief Reads the events of a `text/event-stream` body as its bytes come, as the HTML Living Standard (9.2.6) says a
 * browser does: lines ended by CRLF, LF or CR, fields, comments, a leading BOM, and text that is not UTF-8 read with
 * U+FFFD in its place. It does no IO.
 *
 * @code
 * http::event_parser events;
 *
 * while (auto const piece = co_await response.body.read()) {
 *   events.feed(*piece);
 *
 *   while (auto event = events.next()) {
 *     show(event->data);
 *   }
 * }
 * @endcode
 *
 * An event the stream has not ended with its blank line when it ends is not one, as for a browser. Thread safety: none,
 * one user at a time.
 */
class event_parser {
public:
  /**
   * @brief The most an event holds by default, 1 MiB: its data and its type as read, and the line being read, as it
   * came, its field's name included.
   */
  static constexpr std::size_t default_max_event_size = std::size_t{1} << 20U;

  /** @brief A parser holding at most `max_event_size` bytes of an event, against a peer that never ends one. */
  explicit event_parser(std::size_t max_event_size = default_max_event_size) noexcept
      : max_event_size_(max_event_size) {}

  /**
   * @brief Adds bytes of the body, which next() reads. They are copied. Call next() until it gives nothing before the
   * next piece: the parser then holds no more than a piece and an event, whatever the peer sends.
   *
   * @throws std::system_error with http::error::event_too_large once next() has thrown it, and std::logic_error while
   * next() has not given every event of the bytes fed before.
   */
  void feed(std::string_view bytes);

  /**
   * @brief The next event the bytes fed so far end, or none until more comes.
   *
   * @throws std::system_error with http::error::event_too_large once an event, the line being read included, holds
   * more than the most it may, or an id more than max_id_size, and on every call after.
   */
  [[nodiscard]] std::optional<server_sent_event> next();

  /**
   * @brief The most an `id` holds, 1 KiB: each event carries a copy, and it goes back in a header. Past it, next()
   * fails as for an event too large.
   */
  static constexpr std::size_t max_id_size = 1024;

  /**
   * @brief The ID of the last event the stream ended, empty if none: what `Last-Event-ID` sends when connecting again.
   * An `id` field counts once the blank line after it has come, as for a browser, so that an event cut short is sent
   * again.
   */
  [[nodiscard]] std::string const &last_event_id() const noexcept QUOLL_LIFETIMEBOUND { return last_event_id_; }

  /** @brief The most retry() gives: a longer time the stream asks for is cut to it. */
  static constexpr std::chrono::milliseconds max_retry = std::chrono::hours{24};

  /** @brief How long to wait before connecting again, if the stream said it with a `retry` field: at most max_retry. */
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept { return retry_; }

private:
  // The event a blank line ends, if it has data.
  [[nodiscard]] std::optional<server_sent_event> dispatch();

  // A line without its end, as it came: the event it completes, if any.
  [[nodiscard]] std::optional<server_sent_event> take_line(std::string_view line);

  void check_size(std::size_t held);
  void check_id_size();

  std::size_t max_event_size_;
  // What was fed, from position_ on not read yet.
  std::string buffer_;
  std::size_t position_ = 0;
  // How much of the line being read, from position_, holds no line end: the next search starts past it.
  std::size_t scanned_ = 0;
  // A CR ended the last line: an LF right after it ends no other.
  bool after_cr_ = false;
  // Whether the start of the stream has been read, a BOM there skipped.
  bool started_ = false;
  bool failed_ = false;
  // Whether next() has given every event of what was fed.
  bool drained_ = true;
  std::string data_;
  std::string type_;
  // The id fields read, and what the last event ended took from them.
  std::string id_buffer_;
  // Whether an id field came since the last blank line: only then is it copied.
  bool id_changed_ = false;
  std::string last_event_id_;
  std::optional<std::chrono::milliseconds> retry_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_EVENT_PARSER_HPP
