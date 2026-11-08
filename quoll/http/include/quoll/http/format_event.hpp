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

#ifndef QUOLL_HTTP_FORMAT_EVENT_HPP
#define QUOLL_HTTP_FORMAT_EVENT_HPP

#include <quoll/http/server_sent_event.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace quoll::http {

/**
 * @brief `event` as the text of a `text/event-stream`, its blank line included: `data` goes in as many `data` fields as
 * it has lines, whatever ends them (CRLF, LF or CR), so that a reader gets it back with LF between them.
 *
 * @throws std::invalid_argument for text that is not UTF-8, or a `type` or an `id` holding a CR or an LF, which would
 * end the field, or a NUL in the `id`, which a reader ignores.
 */
[[nodiscard]] std::string format_event(server_sent_event const &event);

/**
 * @brief A `retry` field: how long a reader waits before it connects again, once the stream has ended. It dispatches
 * no event.
 *
 * @throws std::invalid_argument for a negative time.
 */
[[nodiscard]] std::string format_retry(std::chrono::milliseconds delay);

/**
 * @brief A comment, which a reader ignores: what keeps a stream that has nothing to say from looking dead to a proxy
 * between them. `text` may be empty.
 *
 * @throws std::invalid_argument for text that is not UTF-8, or that holds a CR or an LF.
 */
[[nodiscard]] std::string format_comment(std::string_view text = {});

} // namespace quoll::http

#endif // QUOLL_HTTP_FORMAT_EVENT_HPP
