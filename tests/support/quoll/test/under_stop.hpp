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

#ifndef QUOLL_TEST_UNDER_STOP_HPP
#define QUOLL_TEST_UNDER_STOP_HPP

#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/task.hpp>

#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

namespace quoll::test {

// Awaits `work` under `stop_token`: a stop there throws std::errc::operation_canceled, as it did before a stop ended a
// task without an exception (#151), so that a test sees it where it awaits, and tells it from the others. Thrown in
// this task, which runs under the awaiter's token: with that one stopped too, the awaiter ends on the stop.
template <class T>
async::task<T> under_stop(async::task<T> work, std::stop_token stop_token) {
  auto result = co_await async::detail::owned_chain<T>{std::move(work), std::move(stop_token)};

  if (!result) {
    throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::test::under_stop"};
  }

  if constexpr (!std::is_void_v<T>) {
    co_return std::move(*result);
  }
}

} // namespace quoll::test

#endif // QUOLL_TEST_UNDER_STOP_HPP
