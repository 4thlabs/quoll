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

#ifndef QUOLL_ASYNC_WITH_TIMEOUT_HPP
#define QUOLL_ASYNC_WITH_TIMEOUT_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/require_service.hpp>
#include <quoll/async/detail/saturated.hpp>
#include <quoll/async/detail/timeout_children.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>

#include <chrono>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::async {

/**
 * @brief Runs `work`, and requests it to stop if it has not finished after `timeout`, through the timer of the executor
 * it runs on (as quoll::runtime provides it).
 *
 * Returns what `work` returns and rethrows what it throws, with one exception: when the timeout has expired, its
 * std::errc::operation_canceled becomes std::errc::timed_out (a std::system_error). Stopping is cooperative: `work` is
 * always awaited to its end, so that nothing it refers to is left behind, and the timeout only ends it as soon as it
 * observes its stop token; a `work` that finishes anyway still gives its result, as it did the job. A stop request of
 * the awaiter stops `work` too, and stays an operation_canceled. Throws std::logic_error, before running `work`, if the
 * executor offers no timer.
 */
template <class Rep, class Period, class T>
[[nodiscard]] task<T> with_timeout(std::chrono::duration<Rep, Period> asked, task<T> work) {
  auto const timeout = detail::saturated(asked);
  auto const timer =
      detail::require_service<timer_ref>(co_await current_executor(), "quoll::async::with_timeout", "timer");
  auto const stop_token = co_await current_stop_token();
  detail::timeout_state state;
  std::stop_callback const forward_stop{stop_token, detail::timeout_stop_forwarder{&state}};

  try {
    // The work comes first: when it ends before suspending, the timer is not even started.
    [[maybe_unused]] auto results = co_await when_all(detail::timeout_work(std::move(work), state),
                                                      detail::timeout_watchdog(timer, timeout, state));

    if constexpr (!std::is_void_v<T>) {
      co_return std::get<0>(std::move(results));
    }
  } catch (std::system_error const &error) {
    if (state.expired && error.code() == std::errc::operation_canceled) {
      throw std::system_error{std::make_error_code(std::errc::timed_out), "quoll::async::with_timeout"};
    }

    throw;
  }
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_WITH_TIMEOUT_HPP
