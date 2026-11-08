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

#ifndef QUOLL_REST_NEXT_HPP
#define QUOLL_REST_NEXT_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace quoll::rest {

class next;

/**
 * @brief Wraps what a rest::client sends (see client::use): given the request and what sends it on, it returns the
 * response. It may change the request, send it again (rest::retry), or answer without sending it. It takes the request
 * by value, `const &` or `&&`, and `next` by value or `const &`: what it refers to lives until its task ends.
 *
 * std::function rather than std::move_only_function, which Emscripten's libc++ does not have yet: interceptors are
 * copyable, as a copy of the client shares them.
 */
using interceptor = std::function<async::task<http::response>(http::request &&, next const &)>;

namespace detail {

// What a client's interceptors send through: shared by its copies and their calls, immutable once built, so that a call
// keeps it until its task ends, as an http::HttpTransport's task owns what it needs. Each interceptor is shared too, so
// that one added to a copy leaves those before it the same objects, with the state they keep.
struct interception {
  std::vector<std::shared_ptr<interceptor const>> interceptors;
  // The client's transport, a copy of its own.
  std::function<async::task<http::response>(http::request)> send;
};

[[nodiscard]] async::task<http::response> send_through(std::shared_ptr<interception const> through,
                                                       http::request message);

} // namespace detail

/**
 * @brief What comes after an interceptor: the next one, or the client's transport. Called once in general, more to send
 * again, or not at all to answer in its place.
 *
 * Copied freely: a copy shares what it sends through, and the task it returns owns what it needs, so that it may be
 * awaited after the interceptor's own task has ended. Thread safety: its const function may be called from any thread.
 */
class next {
public:
  // A handle on shared state: copies and no moves, so that one moved from stays usable rather than hold a null pointer.
  next(next const &) = default;
  next &operator=(next const &) = default;
  ~next() = default;

  /** @brief The response to `message`. What a later interceptor or the transport throws comes out of the task. */
  [[nodiscard]] async::task<http::response> operator()(http::request message) const;

private:
  friend async::task<http::response> detail::send_through(std::shared_ptr<detail::interception const>, http::request);

  next(std::shared_ptr<detail::interception const> through, std::size_t position) noexcept
      : through_(std::move(through)), position_(position) {}

  // A coroutine of its own, its arguments by value: the next that calls it may go before its task ends.
  [[nodiscard]] static async::task<http::response> send_from(std::shared_ptr<detail::interception const> through,
                                                             std::size_t position, http::request message);

  std::shared_ptr<detail::interception const> through_;
  std::size_t position_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_NEXT_HPP
