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

#ifndef QUOLL_SERVER_DETAIL_CONNECTION_HPP
#define QUOLL_SERVER_DETAIL_CONNECTION_HPP

#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/server/connection_options.hpp>

#include <array>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::server::detail {

// How reading a request ended.
struct request_read {
  std::error_code error;
  // The request did not arrive in time. Told apart from `error`: a stream reports std::errc::timed_out too, for a peer
  // that has vanished, and that one gets no answer.
  bool expired = false;
};

// The status answering a request that could not be read, if it deserves one: a peer that closed in the middle of a
// message, or a broken stream, gets nothing.
[[nodiscard]] inline std::optional<http::status> read_error_status(request_read const &read) noexcept {
  if (read.expired) {
    return http::status::request_timeout;
  }

  if (read.error == http::error::header_too_large) {
    return http::status::request_header_fields_too_large;
  }

  if (read.error == http::error::body_too_large) {
    return http::status::content_too_large;
  }

  if (read.error == http::error::unsupported_method) {
    return http::status::not_implemented;
  }

  if (read.error == http::error::bad_message) {
    return http::status::bad_request;
  }

  return std::nullopt;
}

// Drops what comes before a request and is no part of it: the empty lines a server ignores (RFC 9112, 2.2), which some
// clients send after a body.
inline void skip_empty_lines(std::string &buffer) { buffer.erase(0, buffer.find_first_not_of("\r\n")); }

// Waits for a request to start, in `buffer`. False when the connection ends instead, whatever the reason: it was idle,
// so nothing is owed to the peer.
template <io::ByteStream Stream>
async::task<bool> wait_for_request(Stream &stream, std::string &buffer) {
  std::array<char, 4096> chunk;

  while (buffer.empty()) {
    auto const received = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (received.error) {
      co_return false;
    }

    buffer.append(chunk.data(), received.transferred);
    skip_empty_lines(buffer);
  }

  co_return true;
}

// http::read(), its cancellation thrown: with_timeout() only tells its own from another when it is.
template <io::ByteStream Stream>
async::task<std::error_code> read_or_throw_canceled(Stream &stream, std::string &buffer, http::request_parser &parser) {
  auto const error = co_await http::read(stream, buffer, parser);

  if (error == std::errc::operation_canceled) {
    throw std::system_error{error};
  }

  co_return error;
}

// http::read() of a request, up to the timeout. Streams report their errors as values: thrown, timed_out can only be
// the timeout's.
template <io::ByteStream Stream>
async::task<request_read> read_request(Stream &stream, std::string &buffer, http::request_parser &parser,
                                       connection_options const &options) {
  try {
    co_return request_read{co_await async::detail::with_optional_timeout(
        options.read_timeout, read_or_throw_canceled(stream, buffer, parser))};
  } catch (std::system_error const &thrown) {
    if (thrown.code() == std::errc::timed_out) {
      co_return request_read{thrown.code(), true};
    }

    if (thrown.code() == std::errc::operation_canceled) {
      co_return request_read{thrown.code()};
    }

    throw;
  }
}

// The router's response, or 500 if its handler threw, reported to the application's callback if it gave one.
inline async::task<http::response> respond(http::router const &router, http::request request,
                                           connection_options const &options) {
  try {
    co_return co_await router.dispatch(std::move(request));
  } catch (...) {
    if (options.on_handler_error) {
      options.on_handler_error(std::current_exception());
    }
  }

  co_return http::detail::status_response(http::status::internal_server_error);
}

// Ready to write. A response to HEAD has no body: it announces the length of the one it does not carry, with
// Content-Length rather than chunked coding, whose terminating chunk would be read as the next response. The last
// response of a connection says so.
inline void prepare(http::response &response, bool head, bool keep_alive) {
  if (head) {
    if (response.fields.contains("Transfer-Encoding")) {
      response.fields.erase("Transfer-Encoding");
      response.fields.set("Content-Length", std::to_string(response.body.size()));
    } else if (!response.body.empty() && !response.fields.contains("Content-Length")) {
      response.fields.set("Content-Length", std::to_string(response.body.size()));
    }

    response.body.clear();
    response.trailers.clear();
  }

  if (!keep_alive) {
    response.fields.set("Connection", "close");
  }
}

// Writes the response, or a 500 in its place if it cannot be serialized (a handler built an invalid one).
template <io::ByteStream Stream>
async::task<std::error_code> write_response(Stream &stream, http::response const &response, bool keep_alive) {
  std::optional<async::task<std::error_code>> sending;

  try {
    sending.emplace(http::write(stream, response));
  } catch (std::invalid_argument const &) {
    auto failed = http::detail::status_response(http::status::internal_server_error);

    prepare(failed, false, keep_alive);
    sending.emplace(http::write(stream, failed));
  }

  co_return co_await std::move(*sending);
}

// write_response(), up to the timeout: past it, the error is the stream's cancellation.
template <io::ByteStream Stream>
async::task<std::error_code> send_response(Stream &stream, http::response const &response, bool keep_alive,
                                           connection_options const &options) {
  co_return co_await async::detail::with_optional_timeout(options.write_timeout,
                                                          write_response(stream, response, keep_alive));
}

} // namespace quoll::server::detail

#endif // QUOLL_SERVER_DETAIL_CONNECTION_HPP
