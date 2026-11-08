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

#ifndef QUOLL_HTTP_SERVER_SENT_EVENT_HPP
#define QUOLL_HTTP_SERVER_SENT_EVENT_HPP

#include <cstddef>
#include <optional>
#include <string>

namespace quoll::http {

/** @brief The most an event's `id` holds: one longer is not written, and is ignored when read. */
inline constexpr std::size_t max_event_id_size = 1024;

/**
 * @brief An event of a `text/event-stream` body (Server-Sent Events, HTML Living Standard 9.2).
 *
 * As read by http::event_parser: `type` is `"message"` unless the stream named another, and `id` is the last event ID
 * the stream set, that of this event or of one before it, none if it set none or reset it. As written by
 * format_event: a `"message"` type, or an empty one, is not written, as a reader defaults to `"message"`, and an `id`
 * is written only if set: an empty one resets the reader's last event ID, and reads back as none.
 */
struct server_sent_event {
  /** @brief The type of the event: `"message"`, or what an `event` field named. */
  std::string type = "message";
  /** @brief Its data: the `data` fields of the event, joined with LF. */
  std::string data;
  /** @brief The last event ID, which a reader sends back as `Last-Event-ID` when it connects again. */
  std::optional<std::string> id;

  [[nodiscard]] bool operator==(server_sent_event const &) const = default;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_SERVER_SENT_EVENT_HPP
