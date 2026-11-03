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

#ifndef QUOLL_EMSCRIPTEN_FETCH_TRANSPORT_HPP
#define QUOLL_EMSCRIPTEN_FETCH_TRANSPORT_HPP

#include <quoll/async/task.hpp>
#include <quoll/emscripten/detail/fetch_operation.hpp>
#include <quoll/http/detail/check.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/transport.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::emscripten {

/**
 * @brief HttpTransport over the JavaScript fetch() of the browser, a Worker or Node.
 *
 * Each request goes to `base_url` followed by its target: `https://api.example.com` and `/accounts/42` fetch
 * `https://api.example.com/accounts/42`. An empty base URL keeps the target relative, to the page's origin in a
 * browser. The fields fetch manages itself (Host, Content-Length, Connection and the other hop-by-hop fields) are not
 * sent; the request's trailers neither.
 *
 * The response has the status, status text, fields and complete body that fetch exposes. fetch decodes the body
 * (decompressed, de-chunked), so the fields describing the bytes on the wire (Content-Encoding, Content-Length,
 * Transfer-Encoding) are dropped; the other fields are named in lower case, repeated ones are combined, and the
 * version is not known (`http_1_1`). Unlike a client over a connector, fetch follows redirects itself, possibly to
 * another origin: a browser does not expose the 3xx response.
 *
 * A failure to get a response (network error, refused connection, CORS) throws std::system_error from the task, with
 * `std::errc::io_error` and fetch's message; a stop request aborts the fetch, with `std::errc::operation_canceled`.
 * The task resumes on its awaiter's executor, which must run on the event loop (quoll::emscripten::executor).
 *
 * The task copies what it needs: the transport does not have to outlive it. Thread safety: use it, and request stop,
 * from the thread running the event loop.
 */
class fetch_transport {
public:
  /** @brief A transport sending relative requests, to the page's origin. */
  fetch_transport() = default;

  /**
   * @brief A transport sending requests to `base_url`: a scheme and a host (`https://api.example.com`), optionally a
   * port and a path prefix, without a trailing slash.
   */
  explicit fetch_transport(std::string base_url) noexcept : base_url_(std::move(base_url)) {}

  /**
   * @brief Fetches `request`.
   *
   * @throws std::invalid_argument here, before the task runs, for a target not in origin form (starting with `/`),
   * which could leave the base URL's host (`@evil.example/`), and for what http::serialize() rejects: a target with
   * spaces or line breaks, a field name that is not a token, a value with a line break.
   */
  [[nodiscard]] async::task<http::response> send(http::request request) const {
    if (!request.target.starts_with('/')) {
      throw std::invalid_argument{"quoll::emscripten::fetch_transport: the target is not in origin form"};
    }

    http::detail::check_target(request.target);

    auto url = base_url_ + request.target;
    auto fields = detail::pack_fields(request.fields);

    return detail::fetch(std::move(url), std::move(fields), std::move(request));
  }

private:
  std::string base_url_;
};

static_assert(http::HttpTransport<fetch_transport>);

} // namespace quoll::emscripten

#endif // QUOLL_EMSCRIPTEN_FETCH_TRANSPORT_HPP
