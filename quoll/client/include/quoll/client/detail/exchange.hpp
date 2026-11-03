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

#ifndef QUOLL_CLIENT_DETAIL_EXCHANGE_HPP
#define QUOLL_CLIENT_DETAIL_EXCHANGE_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/detail/connection_pool.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/write.hpp>

#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::client::detail {

// A request serialized by send(), before its task runs, with what the exchange needs to know about it.
struct prepared_request {
  std::string wire;
  http::verb method = http::verb::get;
  // The request lets its connection carry another one (HTTP/1.1, no `Connection: close`).
  bool keep_alive = false;
};

// Methods whose request may be sent twice: the server may have received the first copy before closing. Listed rather
// than excluded, so that a method added to http::verb is not sent twice by default.
[[nodiscard]] constexpr bool idempotent(http::verb method) noexcept {
  switch (method) {
  case http::verb::get:
  case http::verb::head:
  case http::verb::put:
  case http::verb::delete_:
  case http::verb::options:
  case http::verb::trace:
    return true;
  default:
    return false;
  }
}

// Interim responses read before the final one, against a server sending them forever.
inline constexpr int max_interim_responses = 8;

// How a connection kept from an earlier request shows that the server closed it meanwhile.
[[nodiscard]] inline bool closed_meanwhile(std::error_code error) noexcept {
  return error == io::error::end_of_stream || error == std::errc::connection_reset || error == std::errc::broken_pipe;
}

// Sends `request` on `connection` and reads its final response, skipping the interim 1xx ones; the connection goes
// back to `pool` if it can carry another request. With `retry_if_closed` (an idle connection), a connection found
// closed before any response, or answering 408 first (sent by the server before it closed the idle connection), is
// not an error: no response, the caller sends the request again on a new connection. Without it, a response or an
// exception.
inline async::task<std::optional<http::response>> exchange(connection_pool &pool, http::parse_limits limits,
                                                           io::any_stream connection, prepared_request const &request,
                                                           bool retry_if_closed) {
  auto const sent = co_await io::write(connection, std::as_bytes(std::span{request.wire}));

  if (sent.error) {
    if (retry_if_closed && closed_meanwhile(sent.error)) {
      co_return std::nullopt;
    }

    throw std::system_error{sent.error, "quoll::client: cannot send the request"};
  }

  std::string buffer;
  http::response_parser parser{limits};
  auto interim = 0;

  parser.set_request_method(request.method);

  for (;;) {
    if (auto const error = co_await http::read(connection, buffer, parser)) {
      if (retry_if_closed && interim == 0 && closed_meanwhile(error)) {
        co_return std::nullopt;
      }

      throw std::system_error{error, "quoll::client: cannot read the response"};
    }

    auto const keep_alive = parser.keep_alive();
    auto const upgrade = parser.upgrade();
    auto response = parser.release();

    if (http::is_informational(response.status) && !upgrade) {
      if (++interim > max_interim_responses) {
        throw std::system_error{http::error::bad_message, "quoll::client: too many interim responses"};
      }

      continue;
    }

    if (retry_if_closed && interim == 0 && response.status == http::status::request_timeout) {
      co_return std::nullopt;
    }

    // Bytes past the response would be read as the next one: a connection carrying them is not reused.
    if (request.keep_alive && keep_alive && !upgrade && buffer.empty()) {
      pool.give_back(std::move(connection));
    }

    co_return response;
  }
}

} // namespace quoll::client::detail

#endif // QUOLL_CLIENT_DETAIL_EXCHANGE_HPP
