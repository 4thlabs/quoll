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

#ifndef QUOLL_ASYNC_DETACHED_HPP
#define QUOLL_ASYNC_DETACHED_HPP

#include <quoll/async/completion_handler.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/detail/report.hpp>

#include <exception>

namespace quoll::async {

/** @brief Type of async::detached. */
struct detached_t {
  /** @brief A task without a value completed. */
  void operator()() const noexcept {}

  /** @brief A task completed with a value, dropped. */
  template <class T>
  void operator()(T &&) const noexcept {}

  /**
   * @brief A task let an exception escape: it is written to stderr, and the program goes on, as tokio does with a
   * spawned task that panics, so that one task does not take down a server's others. But for
   * std::errc::operation_canceled, whatever caused it: a stop is no failure, and ends the task with no word.
   */
  void operator()(std::exception_ptr failure) const noexcept {
    if (!failure) {
      return;
    }

    if (!detail::is_a_stop(failure)) {
      detail::report("a task spawned with async::detached ended on an exception", failure);
    }
  }
};

/**
 * @brief Completion handler for co_spawn() whose task is on its own, like asio::detached: its value is dropped, and an
 * exception it lets escape is written to stderr, the program going on, unless it is std::errc::operation_canceled, what
 * a stop request throws. To do something else with it, give co_spawn() a handler of your own.
 *
 * @code
 * co_spawn(runtime.get_executor(), serve_notes(8080), quoll::async::detached, runtime.get_stop_token());
 * @endcode
 */
inline constexpr detached_t detached{};

static_assert(CompletionHandler<detached_t, void>);
static_assert(CompletionHandler<detached_t, int>);

} // namespace quoll::async

#endif // QUOLL_ASYNC_DETACHED_HPP
