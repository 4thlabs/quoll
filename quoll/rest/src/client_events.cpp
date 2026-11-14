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

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/is_event_stream.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/client_events.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::rest::detail {

namespace {

constexpr std::size_t max_refusal_body = std::size_t{64} * 1024;

} // namespace

bool is_accepted_stream(http::fields const &fields, std::string_view accepted) {
  if (http::is_event_stream(fields)) {
    return true;
  }

  // A media type of its own counts when the Accept names it alone, and it names an event stream: a third-party API's
  // `application/vnd.example+event-stream`, not `application/json`, which no stream is.
  auto const named = list_elements({accepted});

  if (named.size() != 1 || named.front().contains('*') || !media_type(named.front()).ends_with("event-stream")) {
    return false;
  }

  auto const given = media_type(fields.find("Content-Type").value_or(""));

  return !given.empty() && http::detail::iequals(media_type(named.front()), given);
}

async::task<refusal> refusal_of(http::streamed_response streamed, std::string reason) {
  http::response received;

  received.status = streamed.status;
  received.reason = std::move(streamed.reason);
  received.version = streamed.version;
  received.fields = std::move(streamed.fields);

  // A 200 that is no stream is said by its head: its body, which may never end, is not read.
  if (received.status == http::status::ok) {
    co_return refusal{std::move(received), std::move(reason)};
  }

  // What came of it, if reading it fails: the answer is the status, which has come. At the bound, it reads no
  // further, whether more was to come or not.
  try {
    for (;;) {
      auto const piece = co_await streamed.body.read();

      if (!piece) {
        break;
      }

      auto const room = max_refusal_body - received.body.size();

      received.body.append(piece->substr(0, room));

      if (received.body.size() == max_refusal_body) {
        reason += " (its body read up to 64 KiB)";
        break;
      }
    }
  } catch (std::system_error const &failure) {
    // A stop or a timeout is the caller's: it goes on as from any task, not as the server's answer.
    if (failure.code() == std::errc::operation_canceled || failure.code() == std::errc::timed_out) {
      throw;
    }

    reason += " (its body cut short)";
  }

  co_return refusal{std::move(received), std::move(reason)};
}

} // namespace quoll::rest::detail
