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

#ifndef QUOLL_ASYNC_DETAIL_TIMEOUT_CHILDREN_HPP
#define QUOLL_ASYNC_DETAIL_TIMEOUT_CHILDREN_HPP

#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>

#include <chrono>
#include <optional>
#include <stop_token>
#include <utility>

namespace quoll::async::detail {

// What the two children of a with_timeout() share. Each runs under its own stop source: the end of one stops the other,
// and a stop request of the awaiter stops both.
struct timeout_state {
  std::stop_source work;
  std::stop_source watchdog;
  // Written by the watchdog, read once both children have joined.
  bool expired = false;
};

// Forwards a stop request of the with_timeout() awaiter to its children.
struct timeout_stop_forwarder {
  timeout_state *state;

  void operator()() const noexcept {
    state->work.request_stop();
    state->watchdog.request_stop();
  }
};

// What `work` gives, or std::nullopt once it ended on a stop, of the timeout or of the awaiter: with_timeout() tells
// them apart.
template <class T>
[[nodiscard]] task<std::optional<non_void_t<T>>> timeout_work(task<T> work, timeout_state &state QUOLL_LIFETIMEBOUND) {
  stop_on_exit const stop_watchdog{state.watchdog};

  co_return co_await owned_chain<T>{std::move(work), state.work.get_token()};
}

// A timer that ends on its stop rather than say false is taken for one that has not expired.
[[nodiscard]] inline task<>
timeout_watchdog(timer_ref timer, std::chrono::nanoseconds timeout, timeout_state &state QUOLL_LIFETIMEBOUND) {
  stop_on_exit const stop_work{state.work};

  state.expired = (co_await owned_chain<bool>{timer.wait_for(timeout), state.watchdog.get_token()}).value_or(false);
}

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_TIMEOUT_CHILDREN_HPP
