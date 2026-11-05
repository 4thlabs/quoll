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

#ifndef QUOLL_CLIENT_STREAMED_RESPONSE_HPP
#define QUOLL_CLIENT_STREAMED_RESPONSE_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/version.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::client {

/**
 * @brief The body of a streamed_response, a piece at a time. Copies share what reads it, the connection among them, so
 * that it may be taken out of its response, and the last to go closes the connection if it has not come to its end.
 */
class response_body {
public:
  response_body() noexcept = default;

  /** @brief What only what builds a streamed_response has to give. */
  response_body(http::body_reader reader, std::shared_ptr<void> connection) noexcept
      : reader_(reader), connection_(std::move(connection)) {}

  /** @brief The next piece, valid up to the next read(), or nothing at the end of the body: see http::body_reader. */
  [[nodiscard]] async::task<std::optional<std::string_view>> read() const { return reader_.read(); }

private:
  http::body_reader reader_;
  std::shared_ptr<void> connection_;
};

/**
 * @brief A response whose head has come, and whose body is read a piece at a time: what transport::stream() produces.
 *
 * @code
 * auto response = co_await transport.stream(std::move(request));
 *
 * while (auto const piece = co_await response.body.read()) {
 *   co_await store(*piece);
 * }
 * @endcode
 *
 * Read to its end, or with nothing to read, its connection goes back to the transport. Destroyed before, the connection
 * closes, which is how to give up on a body. Either way, the last copy of `body` goes on the loop of what reads it, as
 * it holds the connection. A failure to read throws std::system_error from `body.read()`, as transport::send() would,
 * with a read timeout of `response_timeout` per piece, and closes the connection, leaving its place to others, as a
 * cancellation does: reading ends there, as a stream does not take it up again.
 *
 * Thread safety: none.
 */
struct streamed_response {
  http::status status = http::status::ok;
  /** @brief Reason phrase as received. */
  std::string reason{};
  http::version version = http::version::http_1_1;
  http::fields fields{};
  response_body body{};
  /** @brief Fields that came after a chunked body, once `body` has come to its end. */
  std::shared_ptr<http::fields const> trailers{};
};

} // namespace quoll::client

#endif // QUOLL_CLIENT_STREAMED_RESPONSE_HPP
