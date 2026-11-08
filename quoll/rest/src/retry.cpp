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

#include <quoll/rest/retry.hpp>

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest/next.hpp>

#include <chrono>
#include <optional>
#include <system_error>
#include <utility>

namespace quoll::rest {

namespace {

[[nodiscard]] bool idempotent(http::request const &message) {
  using http::verb;

  switch (message.method) {
  case verb::get:
  case verb::head:
  case verb::put:
  case verb::delete_:
  case verb::options:
  case verb::trace:
    return !message.body_writer;
  default:
    return false;
  }
}

// What a server says when the failure may be on the way to it, or passing.
[[nodiscard]] bool worth_again(http::status status) {
  return status == http::status::bad_gateway || status == http::status::service_unavailable ||
         status == http::status::gateway_timeout;
}

// By value: the interceptor that started it may go before it ends.
async::task<http::response> retrying(int attempts, std::chrono::milliseconds delay, http::request message, next send) {
  auto wait = delay;

  for (int attempt = 1;; ++attempt) {
    auto const last = attempt >= attempts;
    std::optional<http::response> response;

    try {
      response = co_await send(last ? std::move(message) : message);
    } catch (std::system_error const &error) {
      // A stop request ends the call, as does what failed on the last attempt.
      if (last || error.code() == std::errc::operation_canceled) {
        throw;
      }
    }

    if (response && (last || !worth_again(response->status))) {
      co_return std::move(*response);
    }

    if (wait > std::chrono::milliseconds::zero()) {
      co_await async::sleep_for(wait);
      // Saturated: a delay from outside, doubled as many times as it is asked.
      wait = wait > std::chrono::milliseconds::max() / 2 ? std::chrono::milliseconds::max() : wait * 2;
    }
  }
}

} // namespace

async::task<http::response> retry::operator()(http::request &&message, next const &send) const {
  if (attempts <= 1 || !idempotent(message)) {
    return send(std::move(message));
  }

  return retrying(attempts, delay, std::move(message), send);
}

} // namespace quoll::rest
