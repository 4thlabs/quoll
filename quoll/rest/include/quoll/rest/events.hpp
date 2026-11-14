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

#ifndef QUOLL_REST_EVENTS_HPP
#define QUOLL_REST_EVENTS_HPP

#include <quoll/rest/codec.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/json_codec.hpp>

namespace quoll::rest {

/**
 * @brief A route's result: a stream of server-sent events, each carrying a `T` encoded by `EventCodec` (JSON by
 * default), rather than one body.
 *
 * @code
 * using feed = rest::get<"/changes", rest::events<change>, rest::header<"Last-Event-ID", std::optional<std::string>>>;
 * @endcode
 *
 * The handler takes an rest::event_sink<T> as its last argument, by reference, and sends through it; a client reads
 * them with `subscribe` (see rest::client). What the codec encodes is the data of an event: UTF-8 text, a CR or a CRLF
 * in it read back as an LF, as for any server-sent event; JSON has none. An http_error the handler throws before the
 * stream starts (open() or the first event) answers as from any handler.
 */
template <class T, class EventCodec = json_codec>
struct events {
  static_assert(Codec<EventCodec, T>, "quoll::rest::events: the codec cannot encode and decode the type");

  using part_category = detail::events_part;
  using type = T;
  using codec = EventCodec;
};

} // namespace quoll::rest

#endif // QUOLL_REST_EVENTS_HPP
