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

#include <quoll/async/task.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/byte_stream.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::server::detail {

// The status answering a request that could not be read, if it deserves one: a peer that closed in the middle of a
// message, or a broken stream, gets nothing.
[[nodiscard]] inline std::optional<http::status> read_error_status(std::error_code error) noexcept {
  if (error == http::error::header_too_large) {
    // 431 Request Header Fields Too Large has no name in http::status.
    return static_cast<http::status>(431);
  }

  if (error == http::error::body_too_large) {
    return http::status::content_too_large;
  }

  if (error == http::error::unsupported_method) {
    return http::status::not_implemented;
  }

  if (error == http::error::bad_message) {
    return http::status::bad_request;
  }

  return std::nullopt;
}

[[nodiscard]] inline http::response status_response(http::status code) {
  http::response response;

  response.status = code;
  return response;
}

// The router's response, or 500 if its handler threw.
inline async::task<http::response> respond(http::router const &router, http::request request) {
  try {
    co_return co_await router.dispatch(std::move(request));
  } catch (...) {
    co_return status_response(http::status::internal_server_error);
  }
}

// Ready to write: a response to HEAD announces the length of the body it does not carry, and the last response of a
// connection says so.
inline void prepare(http::response &response, bool head, bool keep_alive) {
  if (head) {
    if (!response.body.empty() && !response.fields.contains("Content-Length") &&
        !response.fields.contains("Transfer-Encoding")) {
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
    auto failed = status_response(http::status::internal_server_error);

    prepare(failed, false, keep_alive);
    sending.emplace(http::write(stream, failed));
  }

  co_return co_await std::move(*sending);
}

// The outcome of a connection spawned by serve(): it ends by itself, and a failure only ends it.
struct ignore_outcome {
  void operator()() const noexcept {}

  void operator()(std::exception_ptr) const noexcept {}
};

} // namespace quoll::server::detail

#endif // QUOLL_SERVER_DETAIL_CONNECTION_HPP
