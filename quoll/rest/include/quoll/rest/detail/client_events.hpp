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

#ifndef QUOLL_REST_DETAIL_CLIENT_EVENTS_HPP
#define QUOLL_REST_DETAIL_CLIENT_EVENTS_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/rest/detail/client_response.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/event_stream.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest::detail {

// What subscribe produces for an endpoint with events<>.
template <class Endpoint>
using event_stream_of = event_stream<typename events_part_of<Endpoint>::type, typename events_part_of<Endpoint>::codec>;

// Whether a 200 is the stream asked for: a `text/event-stream`, or of the one media type the request's Accept named, an
// event stream of a third-party API's own, case and parameters aside.
[[nodiscard]] bool is_accepted_stream(http::fields const &fields, std::string_view accepted);

// What `streamed`, which is no stream of events, is, with `reason` and what came of its body, up to 64 KiB, an error's
// problem rather than a download: none for a 200, which may never end. A stop or a timeout while it reads goes on as
// such.
[[nodiscard]] async::task<unexpected_response> refusal_of(http::streamed_response streamed, std::string reason);

// The events of the response to `request`, or an http::http_error received for any other answer, its body read up to a
// bound.
template <class Route, class Transport>
[[nodiscard]] async::task<event_stream_of<typename Route::endpoint>>
open_events(std::shared_ptr<Transport const> transport, http::request request, std::string accepted) {
  auto streamed = co_await transport->stream(std::move(request));

  if (streamed.status == http::status::ok && is_accepted_stream(streamed.fields, accepted)) {
    co_return event_stream_of<typename Route::endpoint>{http::event_reader{std::move(streamed.body)}};
  }

  auto const status = streamed.status;
  std::string reason;

  if (status == http::status::ok) {
    reason = "not an event stream: Content-Type " + std::string{streamed.fields.find("Content-Type").value_or("none")};
  } else if (status == http::status::no_content) {
    // What a server answers to end a subscription, which a browser does not connect again after.
    reason = "no stream: 204 No Content";
  } else if (has_status(typename Route::responses{}, status)) {
    // Its own or a with<>'s alike: subscribe has no reply<Endpoint> for one and not the other.
    reason = "declared response " + std::to_string(static_cast<int>(status));
  } else {
    reason = unexpected_reason<Route>(status);
  }

  fail(co_await refusal_of(std::move(streamed), std::move(reason)));
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_EVENTS_HPP
