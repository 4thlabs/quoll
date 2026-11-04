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

#ifndef QUOLL_ASYNC_DETAIL_OPTIONAL_TIMEOUT_HPP
#define QUOLL_ASYNC_DETAIL_OPTIONAL_TIMEOUT_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/with_timeout.hpp>

#include <chrono>
#include <utility>

namespace quoll::async::detail {

// with_timeout() for a timeout that comes from options, set by default: zero means none, and an executor without a
// timer (run_loop, a thread pool) has none to apply, which is no error. A negative one has expired already, as
// with_timeout() has it: a deadline computed too late must not lift the limit.
template <class T>
[[nodiscard]] task<T> with_optional_timeout(std::chrono::nanoseconds timeout, task<T> work) {
  auto const executor = co_await current_executor();

  if (timeout == std::chrono::nanoseconds::zero() || !executor.template query<timer_ref>()) {
    co_return co_await std::move(work);
  }

  co_return co_await with_timeout(timeout, std::move(work));
}

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_OPTIONAL_TIMEOUT_HPP
