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

#ifndef QUOLL_HTTP_RESPONSE_WRITER_HPP
#define QUOLL_HTTP_RESPONSE_WRITER_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>

#include <optional>
#include <string_view>

namespace quoll::http {

/**
 * @brief What a handler writes its response through, as its last parameter, for a body in pieces with no lambda around
 * it.
 *
 * @code
 * router.get("/export/{name}", [](std::string name, http::response_writer out) -> task<> {
 *   out.fields.set("Content-Type", "text/csv");
 *
 *   for (auto const &row : load(name)) {
 *     co_await out.write(row);
 *   }
 * });
 * @endcode
 *
 * The handler returns a `task<>`, and runs once the router has answered and the middlewares have returned, awaited by
 * what serves the connection, so that its parameters live until it ends, a reference or a view too. The head leaves
 * with its first write: past that, changing the status or the fields does nothing.
 *
 * So a middleware has returned by then. It does not see the status when `next` returns, nor catch what the handler
 * throws, nor bound it with a timeout around `next`, and what it holds across `next`, a place of a rate limit, is let
 * go before it runs. It adds to the fields, which the handler's replace by name, but for `Set-Cookie` and `Vary`, which
 * add up.
 *
 * What it throws before its first write answers 500, and after it cuts the connection, as for a `response.body_writer`.
 * If it ends without writing, its response has an empty body. To HEAD, it runs up to its first write, which sends the
 * head and throws std::system_error with `std::errc::operation_canceled`, as does any write after that: a handler that
 * lets it through ends. `Content-Length`, a stop request and a write that fails work as for a `response.body_writer`:
 * see http::body_stream.
 *
 * A middleware finds such a handler in `response.late_head_writer`, not in `response.body_writer`, which replaces it if
 * set. To wrap it, it sets the former, with what it writes through passing `head()` on: see http::body_stream.
 *
 * The response holds such a handler, which it runs, so that it may go on after its router. Copies of a router share it,
 * what it captures included. It runs once: run again, it throws std::logic_error.
 *
 * It may read as it writes, one write and one read at a time, from two tasks on as many threads as its executor has.
 * With `Expect: 100-continue`, `100 Continue` leaves before it starts, so that such a route invites its body even if it
 * goes on to answer without it, unless a status of 300 or more is already set: reading then fails at once.
 *
 * Moved, not copied. Take it by value, or pass it on by reference: it does not outlive the handler's task, and write()
 * on one moved from throws std::logic_error. Thread safety: one write() at a time.
 */
class response_writer {
public:
  /** @brief Writes through `out`. A route makes one: a handler is given it. */
  explicit response_writer(body_stream out) noexcept : out_(out) {}

  response_writer(response_writer const &) = delete;
  response_writer &operator=(response_writer const &) = delete;
  response_writer(response_writer &&other) noexcept;
  response_writer &operator=(response_writer &&other) noexcept;

  /** @brief A handler that wrote nothing gives its head here, for a response with an empty body. */
  ~response_writer();

  /** @brief The status of the response, until the first write(). 200 leaves what a middleware set. */
  http::status status = http::status::ok;

  /** @brief The fields of the response, until the first write(). */
  http::fields fields;

  /**
   * @brief Writes `piece`, which stays valid until the task ends, and sends the head with the first one: an empty one
   * sends it at once, for a stream to announce itself before its first data.
   */
  [[nodiscard]] async::task<> write(std::string_view piece QUOLL_LIFETIMEBOUND);

  /** @brief Whether the head has left, with a first write(): its status and fields then do nothing. */
  [[nodiscard]] bool head_sent() const noexcept { return head_given_; }

  /** @brief Whether it writes somewhere: false once moved from. */
  [[nodiscard]] bool writable() const noexcept { return out_.has_value(); }

private:
  void give_head();

  // Empty once moved from.
  std::optional<body_stream> out_;
  bool head_given_ = false;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_RESPONSE_WRITER_HPP
